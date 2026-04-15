# Flat Hash Map + Sorted Intrusive Tree for an HFT Order Queue

---

## 1. Problem Statement & Constraints

An HFT order book must support the following operations with predictable, nanosecond-scale latency:

| Operation | Description |
|---|---|
| `insert(order)` | Place a new limit order at a price level |
| `cancel(order_id)` | Remove an order by ID in O(1) |
| `match(side, qty)` | Walk the best bid/ask, consuming liquidity |
| `top()` | Peek at best price level |
| `modify(order_id, qty)` | Change quantity without re-queuing |

The killer constraint: **no dynamic allocation on the hot path**, sub-microsecond worst-case latency, cache-line awareness, and zero GC pauses. This rules out `std::map`, `std::unordered_map`, and `std::priority_queue` as primary structures.

---

## 2. `ska::flat_hash_map` — Deep Internals

### 2.1 Provenance

`ska::flat_hash_map` is Malte Skarupke's open-addressing hash map (the "bytell" variant, `bytell_hash_map.h`). It is an evolution of Robin Hood hashing with a key structural insight: metadata and payload are stored in **separate parallel arrays**, giving it SIMD-friendly probing and cache behavior superior to `std::unordered_map`'s node-based chaining.

### 2.2 Memory Layout

```
┌──────────────────────────────────────────────────────────┐
│  info[]  (1 byte per slot)                               │
│  ┌──┬──┬──┬──┬──┬──┬──┬──┐                               │
│  │i0│i1│i2│i3│i4│i5│i6│i7│  ← packed info bytes          │
│  └──┴──┴──┴──┴──┴──┴──┴──┘                               │
│                                                          │
│  entries[] (sizeof(K)+sizeof(V) per slot, aligned)       │
│  ┌────────┬────────┬────────┬────────┐                   │
│  │ e0     │ e1     │ e2     │ e3     │                   │
│  └────────┴────────┴────────┴────────┘                   │
└──────────────────────────────────────────────────────────┘
```

Each **info byte** encodes:
- **bit 7**: "jump-over" flag (whether this slot is part of a probe chain that skips this slot)
- **bits 6-0**: distance from the ideal bucket (Robin Hood displacement, 0–127)

A value of `0` in the info byte means **empty**.

### 2.3 Lookup Path (Robin Hood / Bytell variant)

```
hash(key) → ideal_slot h
probe distance d = 0

loop:
    if info[h + d] == 0 → NOT FOUND
    if info[h + d].distance < d → NOT FOUND  ← Robin Hood invariant
    if entries[h + d].key == key → FOUND
    d++
```

The Robin Hood invariant means the probe sequence is **short and bounded**. The expected probe length is O(1) with very low variance — typically 1–2 comparisons for load factors ≤ 0.875.

### 2.4 Why It Beats `std::unordered_map` for HFT

| Property | `std::unordered_map` | `ska::flat_hash_map` |
|---|---|---|
| Memory model | Heap-allocated nodes | Flat contiguous arrays |
| Cache behavior | Pointer chasing | Sequential scan |
| Allocation on insert | Yes (malloc per node) | Amortized (only on resize) |
| Deletion | O(1), no tombstone | O(1), uses backward-shift |
| Iterator invalidation | Only on rehash | On rehash OR insert |

For an HFT cancel path: `cancel(order_id)` hits the flat map, gets a **pointer/index** into the intrusive tree node directly — zero allocations, one cache-line read.

### 2.5 Pre-sizing and Reservation

```cpp
// At system startup, pre-size for max expected live orders
// ska::flat_hash_map uses power-of-2 bucket counts
order_map.reserve(1 << 17); // 131072 slots, load ~0.5 at 65k orders
```

With the map pre-reserved, **no rehash occurs on the hot path**. This is non-negotiable for HFT.

---

## 3. The Sorted Intrusive Tree — Deep Internals

### 3.1 What "Intrusive" Means

In a non-intrusive container (e.g., `std::set<Order*>`), the tree nodes (`rb_node`, `avl_node`) are **allocated separately** from your data and contain pointers back to it. In an intrusive container, the tree bookkeeping fields (`left`, `right`, `parent`, `color`/`balance`) live **inside your `Order` struct itself**.

```cpp
// Non-intrusive: two heap objects per order
struct RBNode { RBNode* left; RBNode* right; RBNode* parent; bool red; };
struct Order { uint64_t id; double price; int qty; };
// std::set stores RBNode* which points to a separately alloc'd Order

// Intrusive: ONE object, zero extra allocation
struct Order {
    // --- tree bookkeeping (intrusive hooks) ---
    Order*   left;
    Order*   parent;
    Order*   right;
    bool     red;          // or int8_t balance for AVL
    uint8_t  _pad[7];

    // --- business data ---
    uint64_t order_id;
    int64_t  price;        // fixed-point, e.g. price * 10000
    int32_t  qty;
    Side     side;
    uint8_t  _pad2[3];
};
```

Libraries like **Boost.Intrusive** (`boost::intrusive::set`) or **Linux kernel's `rbtree`** implement this pattern. You can also implement it manually (shown below).

### 3.2 Tree Discipline: Red-Black vs. AVL

For an order book:

- **Red-Black Tree** is preferred. It offers **O(log n) worst-case** for insert/delete with at most 2 rotations on delete (vs. O(log n) rotations for AVL). The lower constant on structural mutations matters more than AVL's tighter balance (which gives slightly faster lookups). Linux's `rbtree.h` and Boost.Intrusive use RB trees for exactly this reason.
- **AVL Tree** gives a height bound of `1.44 log₂(n)` vs RB's `2 log₂(n)`, but requires tracking balance factors and more rotations on delete — bad for the cancel/match hot paths.

### 3.3 Price Level Ordering

The tree is keyed on **price** (fixed-point integer, e.g., `int64_t price_ticks`). For the **ask side**, the tree is a min-heap (lowest ask at root left-most). For the **bid side**, it is effectively a max-tree (highest bid). You implement this via comparator inversion or by maintaining two separate trees.

```
Ask-side RB-tree (min at leftmost node):

            100.05
           /      \
       100.02    100.08
       /    \
   100.01  100.03
```

`top()` = leftmost node = `O(log n)` walk, or O(1) if you cache a `min_node*` pointer (updated on insert/delete — this is standard practice).

### 3.4 Price Level Nodes vs. Order Nodes

There are two designs:

**Design A — Tree of Price Levels (recommended)**

```cpp
struct PriceLevel {
    int64_t  price;
    // intrusive RB hooks
    PriceLevel *left, *right, *parent;
    bool      red;
    // per-level order queue (FIFO, intrusive doubly-linked list)
    Order*   head;   // oldest (first to match)
    Order*   tail;   // newest
    int32_t  total_qty;
    int32_t  order_count;
};
```

- The RB tree is keyed on `price`, with `O(log P)` operations where P = number of distinct price levels (typically small, often < 1000 for liquid instruments).
- Within each `PriceLevel`, orders are stored in a **FIFO intrusive doubly-linked list** (price-time priority, standard for equities).

**Design B — Tree of Individual Orders**

Each `Order` is an RB node keyed on `(price, timestamp)`. Simpler but `O(log N)` where N = total live orders, and iteration for matching is slightly more complex.

Design A is almost universally preferred in production because price level count is small and bounded, matching iteration is a tight linked-list walk (cache-friendly), and total_qty per level is maintained for quick depth-of-book queries.

### 3.5 Intrusive Linked List within Each Price Level

```cpp
struct Order {
    // RB tree hooks (for cancel via flat_hash_map lookup)
    // Not in the tree directly — Order lives in PriceLevel's list
    Order*    prev;      // FIFO list prev
    Order*    next;      // FIFO list next
    PriceLevel* level;  // back-pointer to owning PriceLevel

    uint64_t  order_id;
    int64_t   price;
    int32_t   qty;
    Side      side;
    uint8_t   status;   // LIVE, CANCELLED, FILLED
};
```

---

## 4. The Combined Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                      ORDER BOOK                                  │
│                                                                  │
│  ska::flat_hash_map<uint64_t, Order*>  order_index               │
│  (order_id → raw pointer into pre-allocated Order pool)          │
│                                                                  │
│  RB Tree (ask side)          RB Tree (bid side)                  │
│  ┌──────────────┐            ┌──────────────┐                    │
│  │ PriceLevel   │            │ PriceLevel   │                    │
│  │ 100.05       │            │  99.98       │                    │
│  │ [o3]→[o7]    │            │ [o1]→[o4]    │                    │
│  ├──────────────┤            ├──────────────┤                    │
│  │ 100.02       │            │  99.95       │                    │
│  │ [o2]         │            │ [o5]→[o6]    │                    │
│  └──────────────┘            └──────────────┘                    │
│                                                                  │
│  PriceLevel pool: pre-allocated slab                             │
│  Order pool:      pre-allocated slab                             │
└──────────────────────────────────────────────────────────────────┘
```

### 4.1 Memory Pools (Non-Negotiable for HFT)

```cpp
// Fixed-size slab allocators — NO malloc on hot path
template<typename T, size_t Capacity>
class SlabPool {
    alignas(64) T storage[Capacity];
    T* free_list[Capacity];
    size_t free_top = 0;
public:
    SlabPool() { for (size_t i = 0; i < Capacity; ++i) free_list[i] = &storage[i]; free_top = Capacity; }
    T* acquire() noexcept { return (free_top > 0) ? free_list[--free_top] : nullptr; }
    void release(T* p) noexcept { free_list[free_top++] = p; }
};

SlabPool<Order,      1 << 17> order_pool;       // 131072 orders
SlabPool<PriceLevel, 1 << 12> level_pool;       // 4096 price levels
```

`alignas(64)` pins the slab to cache-line boundaries. All `Order*` pointers stored in `ska::flat_hash_map` point into this slab — no heap fragmentation, no allocator lock contention.

---

## 5. Operation Implementations

### 5.1 `insert(order_id, price, qty, side)`

```
1. Acquire Order* from order_pool         → O(1), no malloc
2. Populate Order fields
3. Lookup PriceLevel in RB tree by price  → O(log P)
   3a. If not found: acquire PriceLevel* from level_pool,
       insert into RB tree               → O(log P) + ≤2 rotations
4. Append Order to PriceLevel FIFO tail  → O(1)
5. Update level->total_qty, order_count  → O(1)
6. Insert (order_id → Order*) into ska::flat_hash_map → O(1) amortized
7. Update best_bid/best_ask cache if needed → O(1)
```

Total: **O(log P)** — with P typically < 1000, this is ~10 comparisons.

### 5.2 `cancel(order_id)`

```
1. flat_hash_map::find(order_id)          → O(1), ~1–2 probes
   → yields Order* directly (no second lookup)
2. Unlink Order from PriceLevel FIFO     → O(1), pointer surgery
   order->prev->next = order->next
   order->next->prev = order->prev
3. Decrement level->total_qty, order_count → O(1)
4. If level->order_count == 0:
   Remove PriceLevel from RB tree        → O(log P) + ≤3 rotations
   Release PriceLevel back to level_pool → O(1)
5. flat_hash_map::erase(order_id)        → O(1)
   (uses backward-shift deletion, no tombstone)
6. Release Order* back to order_pool     → O(1)
7. Update best_bid/best_ask if needed    → O(1) or O(log P) if level removed
```

Total: **O(1)** in the common case (level still has other orders), **O(log P)** if the price level is vacated.

### 5.3 `match(side, fill_qty)` — The Critical Path

```
1. Walk best_ask (or best_bid) → O(1) via cached min_node*
2. For each PriceLevel from best price outward:
   Iterate FIFO list head-to-tail:
     reduce order->qty, fill_qty
     if order fully filled:
       unlink from list, erase from flat_hash_map, return to pool
     if fill_qty == 0: stop
   if level empty: remove from RB tree, update best price cache
```

The matching loop is a **pointer-chasing linked list walk** within a price level — but since orders at the same price level are contiguous in the slab (temporal locality from sequential pool allocation), this is cache-warm in practice.

### 5.4 `modify(order_id, new_qty)`

For simple quantity reduction (no price change):
```
1. flat_hash_map::find(order_id) → Order*  O(1)
2. level->total_qty -= (order->qty - new_qty)
3. order->qty = new_qty
```
**O(1)**, fully in-place. For price modification, treat as cancel + insert.

---

## 6. Cache Line Optimization Details

### 6.1 Order Struct Layout (64-byte = 1 cache line)

```cpp
struct alignas(64) Order {
    // Hot fields (read on every match iteration) — first 32 bytes
    Order*      next;          // 8 bytes  — FIFO list traversal
    Order*      prev;          // 8 bytes  — for O(1) unlink
    int64_t     price;         // 8 bytes  — for level validation
    int32_t     qty;           // 4 bytes  — filled on match
    uint32_t    status;        // 4 bytes  — LIVE/CANCELLED/PARTIAL

    // Warm fields (read on cancel, insert) — next 32 bytes
    PriceLevel* level;         // 8 bytes  — back-pointer for O(1) cancel
    uint64_t    order_id;      // 8 bytes  — for map erase
    int64_t     timestamp_ns;  // 8 bytes  — for audit/logging
    Side        side;          // 1 byte
    uint8_t     _pad[7];       // padding to 64 bytes
};
static_assert(sizeof(Order) == 64);
```

With this layout, a match walk touching `next` and `qty` always fits in one cache line.

### 6.2 PriceLevel Struct (64-byte)

```cpp
struct alignas(64) PriceLevel {
    // RB tree hooks — 32 bytes
    PriceLevel* left;
    PriceLevel* right;
    PriceLevel* parent;
    uint32_t    color;    // 0=black, 1=red
    uint32_t    _pad0;

    // Level data — 32 bytes
    int64_t     price;
    Order*      head;         // FIFO front (match here first)
    Order*      tail;         // FIFO back (new orders appended)
    int32_t     total_qty;
    int32_t     order_count;
};
static_assert(sizeof(PriceLevel) == 64);
```

### 6.3 `ska::flat_hash_map` Key/Value Sizing

Use `order_id → Order*` (8-byte key, 8-byte value). Each entry is 16 bytes. The info byte array is separate. At 65k live orders, the entry array is ~1MB — fits in L2/L3. The info byte array is 64KB — fits in L1 on modern server CPUs (typically 32–48KB L1d).

---

## 7. RB Tree Implementation Notes

If you implement the intrusive RB tree manually (rather than using Boost.Intrusive):

### 7.1 Rotation (Left)

```cpp
void rotate_left(PriceLevel*& root, PriceLevel* x) {
    PriceLevel* y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    if (!x->parent)          root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else                           x->parent->right = y;
    y->left = x;
    x->parent = y;
}
```

No allocation. Pure pointer surgery. ~10 instructions on x86-64.

### 7.2 Insert Fixup

Standard RB fixup: after BST insert, walk up and recolor/rotate to restore the RB invariants. At most **2 rotations** for insert.

### 7.3 Delete Fixup

The double-black fixup for delete is the most complex part. At most **3 rotations**. Boost.Intrusive's implementation is a good reference — it is production-hardened and branch-minimized.

### 7.4 Min-Node Cache

```cpp
struct OrderBook {
    PriceLevel* ask_root     = nullptr;
    PriceLevel* ask_min      = nullptr;  // cached best ask
    PriceLevel* bid_root     = nullptr;
    PriceLevel* bid_max      = nullptr;  // cached best bid

    PriceLevel* find_min(PriceLevel* node) {
        while (node->left) node = node->left;
        return node;
    }
    // Updated on every insert/delete that could affect the extremum
};
```

`top()` is then a single pointer dereference — **O(1)**.

---

## 8. Threading Model

HFT systems typically use a **single-threaded, core-pinned** event loop per instrument or per exchange gateway. This eliminates all locking overhead from the order book entirely. The flat_hash_map and RB tree are **not thread-safe** and are not expected to be — the architecture is:

```
[NIC] → [Kernel bypass / DPDK / Solarflare OpenOnload]
      → [Single pinned core, busy-poll loop]
      → [OrderBook (flat_hash_map + intrusive RB tree)]
      → [Strategy engine]
      → [Order sender]
```

If cross-thread access is required (e.g., risk engine reading the book), use a **seqlock** or **snapshot copy** pattern rather than mutexes.

---

## 9. Benchmarks & Latency Profile (Representative)

| Operation | Typical latency (single core, L1/L2 warm) |
|---|---|
| `insert` (existing level) | 80–150 ns |
| `insert` (new price level) | 150–300 ns |
| `cancel` (level survives) | 40–80 ns |
| `cancel` (level vacated) | 100–200 ns |
| `match` (per order consumed) | 20–50 ns |
| `top()` | < 5 ns |

The flat_hash_map lookup (step 1 of cancel) typically costs **~15–25 ns** — roughly 1 L1 hit for info byte + 1 L1/L2 hit for the entry — which is the dominant cost of a cancel when the level survives.

---

## 10. Summary of Design Rationale

| Design Choice | Reason |
|---|---|
| `ska::flat_hash_map` over `std::unordered_map` | Flat arrays, no per-node heap alloc, Robin Hood probing, backward-shift delete (no tombstones) |
| Intrusive RB tree over `std::map` | Zero extra allocation, pointer to tree node IS the Order object, no indirection |
| PriceLevel tree + per-level FIFO list | Price level count is small (O(log P) is ~10 ops); matching is a cache-warm list walk |
| Slab allocators | Deterministic latency, zero malloc on hot path, pool locality |
| 64-byte aligned structs | One cache line per node, prevents false sharing, SIMD-friendly |
| Min-node pointer cache | O(1) best price access for top-of-book queries |
| Single-threaded core-pinned | Eliminates all synchronization overhead |

This combination gives you a **cancel in ~50 ns, insert in ~100–150 ns, and match throughput of ~20–50 ns/order** — which is competitive with production HFT order book implementations.


---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Describe the ska::flat_hash_map + Sorted Intrusive Tree for the order queue in the high-frequency trading system in depth. This description is intended for a computer science expert who is willing to implement this piece of code.
