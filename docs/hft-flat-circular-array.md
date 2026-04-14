# Flat Circular Array Order Queue for HFT

## Conceptual Overview

In high-frequency trading, the order book must handle tens of thousands of insertions, cancellations, and matches per second with sub-microsecond latency. The canonical approach is to replace tree-based or hash-based price-level structures with a **flat circular array** (also called a *price ladder* or *ring buffer price map*) where each slot corresponds to exactly one price level, addressed by a simple integer offset from a sliding base price. The result is O(1) insert, cancel, and best-bid/ask lookup with no heap allocation, no hashing, and cache-friendly sequential access.

---

## Memory Layout

```
 base_price = 10000  (in ticks)
 slot_count = 1024   (must be a power of two)
 tick_size  = 0.01

 Physical array (ring buffer of pointers/indices into order pool):
 ┌───┬───┬───┬───┬───┬────────────────────────────┬────┐
 │ 0 │ 1 │ 2 │ 3 │ 4 │  ...  1019  1020  1021 ... │1023│
 └───┴───┴───┴───┴───┴────────────────────────────┴────┘
   ↑                                                 ↑
  slot for                                        slot for
price 10000                                    price 10000 +
                                              (1023 * tick)

 Mapping: slot = (price_in_ticks - base_price) & (slot_count - 1)
```

Each slot heads a **doubly-linked intrusive list** (or an intrusive FIFO queue) of resting orders at that price level, living in a separately managed, pre-allocated order pool.

---

## Core Fields

```c
typedef struct {
    /* --- hot cache line (64 bytes) --- */
    int64_t   base_price;      // current base in ticks (slides with market)
    uint32_t  slot_count;      // power-of-two capacity
    uint32_t  slot_mask;       // slot_count - 1, for cheap modulo
    int64_t   best_bid_tick;   // cached best bid (absolute ticks)
    int64_t   best_ask_tick;   // cached best ask (absolute ticks)
    uint64_t  _pad[2];

    /* --- per-slot level metadata (separate array, one entry per slot) --- */
    LevelMeta *levels;         // aligned to cache line, size = slot_count

    /* --- order storage --- */
    Order     *order_pool;     // flat pre-allocated slab
    uint32_t   pool_head;      // free-list head index
} PriceLadder;

typedef struct {
    uint32_t  head_order_idx;  // index into order_pool, UINT32_MAX = empty
    uint32_t  tail_order_idx;
    int64_t   total_qty;       // aggregate visible quantity at this level
    uint32_t  order_count;
    uint32_t  _pad;
} LevelMeta;                   // 24 bytes — fits 2-3 per cache line

typedef struct {
    int64_t   price_tick;
    int64_t   qty;
    uint64_t  order_id;
    uint32_t  next_idx;        // intrusive linked list
    uint32_t  prev_idx;
    uint64_t  timestamp_ns;
    uint32_t  participant_id;
    uint8_t   side;            // 0=bid, 1=ask
    uint8_t   flags;
    uint16_t  _pad;
} Order;                       // 48 bytes — fits neatly in cache lines
```

---

## Address Computation

The key insight is replacing expensive modulo with a bitmask:

```c
// Convert an absolute price (in ticks) to a slot index.
// Precondition: |price_tick - base_price| < slot_count
static inline uint32_t price_to_slot(const PriceLadder *L, int64_t price_tick) {
    return (uint32_t)(price_tick - L->base_price) & L->slot_mask;
}

// Convert a slot index back to an absolute price.
static inline int64_t slot_to_price(const PriceLadder *L, uint32_t slot) {
    // Reconstruct sign-correctly using the current base
    int64_t raw = (int64_t)slot - ((int64_t)L->base_price & L->slot_mask);
    return L->base_price + raw;  // only valid when slot is occupied
}
```

Because `slot_count` is a power of two, `& slot_mask` is a single AND instruction — no division, no branch.

---

## The Circular / Sliding Window Invariant

The ladder does **not** represent an infinite price space. It represents a window of `slot_count` consecutive tick levels whose position slides with the market. The invariant is:

```
All live orders satisfy:
    base_price <= order.price_tick < base_price + slot_count
```

When the market drifts and a new order arrives outside the window, the base must **slide**:

```c
void slide_base(PriceLadder *L, int64_t new_order_tick) {
    // Determine how many slots to shift (always a whole number of slots)
    int64_t delta = new_order_tick - L->base_price;

    if (delta >= 0 && delta < (int64_t)L->slot_count) return; // already in range

    // Shift base so the new price lands at slot slot_count/2 (centre the window)
    int64_t new_base = new_order_tick - (int64_t)(L->slot_count / 2);
    int64_t shift    = new_base - L->base_price;

    // Evict any orders whose slots are about to be overwritten.
    // In practice, a well-tuned slot_count means this is extremely rare
    // (handled by cancelling stale far-from-market orders proactively).
    if (shift > 0) {
        for (int64_t i = L->base_price; i < new_base; i++)
            evict_level(L, price_to_slot(L, i));
    } else {
        for (int64_t i = new_base + L->slot_count; i < L->base_price + L->slot_count; i++)
            evict_level(L, price_to_slot(L, i));
    }

    L->base_price = new_base;
}
```

In practice `slot_count = 4096` or `8192` ticks covers the entire realistic trading range of most instruments (e.g., ±$40 at a $0.01 tick), making sliding a rare cold-path event.

---

## Core Operations

### Insert Order — O(1)

```c
void insert_order(PriceLadder *L, Order *o) {
    slide_base(L, o->price_tick);          // no-op 99.999% of the time

    uint32_t   slot  = price_to_slot(L, o->price_tick);
    LevelMeta *level = &L->levels[slot];
    uint32_t   idx   = alloc_order(L, o); // O(1) from free-list

    // Append to tail (FIFO price-time priority)
    if (level->head_order_idx == UINT32_MAX) {
        level->head_order_idx = level->tail_order_idx = idx;
        L->order_pool[idx].next_idx = L->order_pool[idx].prev_idx = UINT32_MAX;
    } else {
        L->order_pool[level->tail_order_idx].next_idx = idx;
        L->order_pool[idx].prev_idx = level->tail_order_idx;
        L->order_pool[idx].next_idx = UINT32_MAX;
        level->tail_order_idx = idx;
    }

    level->total_qty   += o->qty;
    level->order_count += 1;

    // Update best bid/ask
    if (o->side == BID && o->price_tick > L->best_bid_tick) L->best_bid_tick = o->price_tick;
    if (o->side == ASK && o->price_tick < L->best_ask_tick) L->best_ask_tick = o->price_tick;
}
```

### Cancel Order — O(1) with Order Index

Cancellation is O(1) **only** if the system maintains a side-table mapping `order_id → pool_index`. Given that index:

```c
void cancel_order(PriceLadder *L, uint32_t order_idx) {
    Order     *o     = &L->order_pool[order_idx];
    uint32_t   slot  = price_to_slot(L, o->price_tick);
    LevelMeta *level = &L->levels[slot];

    // Unlink from doubly-linked list — O(1)
    if (o->prev_idx != UINT32_MAX) L->order_pool[o->prev_idx].next_idx = o->next_idx;
    else                           level->head_order_idx = o->next_idx;
    if (o->next_idx != UINT32_MAX) L->order_pool[o->next_idx].prev_idx = o->prev_idx;
    else                           level->tail_order_idx = o->prev_idx;

    level->total_qty   -= o->qty;
    level->order_count -= 1;

    free_order(L, order_idx);  // push back to free-list head

    // Lazily update best bid/ask only when the best level is emptied
    if (level->order_count == 0) refresh_best(L, o->side);
}
```

### Match (Top-of-Book Consume) — O(1) amortised

```c
Order *peek_best_bid(PriceLadder *L) {
    LevelMeta *level = &L->levels[price_to_slot(L, L->best_bid_tick)];
    if (level->head_order_idx == UINT32_MAX) return NULL;
    return &L->order_pool[level->head_order_idx];
}

void consume_best_bid(PriceLadder *L, int64_t fill_qty) {
    int64_t    tick  = L->best_bid_tick;
    uint32_t   slot  = price_to_slot(L, tick);
    LevelMeta *level = &L->levels[slot];
    Order     *head  = &L->order_pool[level->head_order_idx];

    head->qty      -= fill_qty;
    level->total_qty -= fill_qty;

    if (head->qty == 0) cancel_order(L, level->head_order_idx);
    if (level->order_count == 0) step_best_bid(L); // walk down one tick
}
```

`step_best_bid` scans downward from `best_bid_tick` — this is O(gap) in the worst case but O(1) amortised because the best price rarely jumps multiple levels at once.

---

## Cache Behaviour and Memory Sizing

| Structure | Size | Placement |
|---|---|---|
| `PriceLadder` hot fields | 64 B | L1 cache line, pinned |
| `LevelMeta[4096]` | 96 KB | fits in L2 (256 KB typical) |
| `Order` pool (50k orders) | ~2.3 MB | L3 or NUMA-local DRAM |
| `order_id → pool_idx` map | ~800 KB | L3 |

The critical insight: the **entire level metadata array for a typical instrument fits in L2 cache**. A match or insert touches exactly two cache lines (the `PriceLadder` hot struct + one `LevelMeta` entry), making the common path essentially a sequence of L2 hits.

---

## Distinguishing Design Choices

**Why power-of-two slot count?**
The `& mask` replaces `% slot_count` — on modern x86 this saves ~20–40 cycles (integer division latency).

**Why intrusive linked lists instead of secondary arrays?**
Order nodes live in a single flat slab. The `next`/`prev` fields are embedded directly in `Order`, avoiding a pointer indirection to a separate container node. The free-list reuses the same `next_idx` field.

**Why separate `LevelMeta` from `Order`?**
Hot access pattern for market data (feed handlers, risk checks) only needs `total_qty` and `order_count`, not order details. Keeping `LevelMeta` in a tight array maximises prefetch efficiency for top-of-book scans.

**Why cache `best_bid_tick` / `best_ask_tick` explicitly?**
Finding the best price by scanning the array would be O(slot_count). The cached scalar fields make top-of-book lookup a single load — the most latency-critical operation in the system.

**Why doubly-linked list at each level?**
FIFO price-time priority requires O(1) dequeue from the head (match) and O(1) cancel from an arbitrary position. A singly-linked list would make mid-list cancellation O(n) per level.

---

## Thread Safety Considerations

In a single-threaded exchange matching engine (the dominant HFT architecture), no locking is needed. In a multi-threaded setup (e.g., separate feed handler and matching threads), the typical approach is:

- **One writer** (matching engine) owns the ladder exclusively.
- **Readers** (risk, market data publisher) access a **shadow copy** updated via `memcpy` of only the `LevelMeta` array after each event, protected by a seqlock — one 64-bit sequence counter, no mutex.

---

## Summary of Complexity

| Operation | Time | Notes |
|---|---|---|
| Insert | O(1) | 2 cache lines touched |
| Cancel | O(1) | requires `order_id → idx` side-table |
| Best bid/ask lookup | O(1) | single scalar load |
| Match / fill | O(1) amortised | step on level exhaustion |
| Base slide | O(k) | k = slots evicted, cold path |
| Level quantity sum | O(1) | `LevelMeta.total_qty` |

This structure is the backbone of virtually every co-located exchange matching engine and the order books of institutional HFT firms — its performance characteristics are simply unapproachable by any tree or hash-based alternative at this latency tier.

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Describe the flat circular array indexed by price offset from a base price for the order queue in the high-frequency trading system in depth. This description is intended for a computer science expert who is willing to implement this piece of code.
