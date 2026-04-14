# Matching Engine Architecture for High-Frequency Trading

## 1. System Overview

A matching engine is the core component of an exchange or trading system. Its job is singular but extraordinarily demanding: accept incoming orders, maintain a limit order book (LOB), and match buy/sell orders according to a price-time priority (FIFO) algorithm — all in **sub-microsecond time**, with minimal jitter and zero dynamic memory allocation in the hot path.

The two dominant performance axioms are:

- **No heap allocation on the critical path.** Every `new`/`delete` or `malloc`/`free` call is a latency landmine — it touches the OS, triggers locks in the allocator, and pollutes cache lines.
- **Cache is everything.** L1 cache hit (~4 cycles), L2 (~12 cycles), L3 (~40 cycles), DRAM (~200 cycles). Every design decision must be evaluated through this lens.

---

## 2. Core Data Structures

### 2.1 The Order Object

The order is the atomic unit. It must be cache-line-friendly:

```cpp
// Align to a cache line to prevent false sharing
struct alignas(64) Order {
    uint64_t order_id;       // 8 bytes — unique monotonic ID
    uint64_t timestamp_ns;   // 8 bytes — TSC-based nanosecond timestamp
    int64_t  price;          // 8 bytes — fixed-point, e.g. price * 1e6
    uint32_t quantity;       // 4 bytes — remaining quantity
    uint32_t initial_qty;    // 4 bytes — original quantity
    uint32_t trader_id;      // 4 bytes
    uint16_t instrument_id;  // 2 bytes
    uint8_t  side;           // 1 byte — BID=0, ASK=1
    uint8_t  type;           // 1 byte — LIMIT, MARKET, IOC, FOK, etc.
    uint8_t  status;         // 1 byte — LIVE, PARTIAL, FILLED, CANCELLED
    uint8_t  _pad[13];       // padding to 64 bytes
};
static_assert(sizeof(Order) == 64, "Order must be exactly one cache line");
```

**Why fixed-point integers for price?** Floating-point comparisons are non-deterministic in cycle count due to denormals and FPU pipeline stalls. A `int64_t` comparison is always one cycle.

---

### 2.2 The Order Pool — Arena Allocation

Instead of `new Order(...)`, you pre-allocate a large contiguous array of orders at startup. This is your slab allocator:

```cpp
template<std::size_t Capacity>
class OrderPool {
    // A bitset tracks which slots are free.
    // Using a flat array ensures spatial locality.
    alignas(64) Order   storage_[Capacity];
    uint32_t            free_stack_[Capacity];
    uint32_t            top_ = 0;

public:
    OrderPool() {
        // Pre-populate the free stack
        for (uint32_t i = 0; i < Capacity; ++i)
            free_stack_[i] = i;
        top_ = Capacity;
    }

    Order* acquire() noexcept {
        if (__builtin_expect(top_ == 0, 0)) return nullptr; // hot path hint
        return &storage_[free_stack_[--top_]];
    }

    void release(Order* o) noexcept {
        free_stack_[top_++] = static_cast<uint32_t>(o - storage_);
    }
};
```

- **O(1) acquire/release**, branchless on the happy path.
- All `Order` objects live in a single contiguous region — prefetcher-friendly.
- `Capacity` is typically `1 << 20` (1M orders), sized to fit in LLC.

---

### 2.3 The Price Level — The Building Block of the Book

Each distinct price point on one side of the book is a **Price Level**, containing a FIFO queue of orders at that price:

```cpp
struct PriceLevel {
    int64_t  price;
    uint64_t total_quantity;  // aggregate quantity at this level (for depth feed)

    // Intrusive doubly-linked list of Orders at this price.
    // Order nodes carry prev/next pointers directly — no separate node allocation.
    Order*   head;  // oldest order (matched first)
    Order*   tail;  // newest order (appended here)
    uint32_t order_count;
};
```

**Intrusive linked list for the order queue:** Rather than `std::list<Order*>` (which allocates a list node per element), embed `prev` and `next` pointers directly inside `Order`. This halves pointer indirections and keeps orders and their queue metadata in the same cache line.

Add to `Order`:
```cpp
Order* prev_at_level;  // 8 bytes
Order* next_at_level;  // 8 bytes
```

Enqueue/dequeue becomes:
```cpp
// O(1), zero allocation
void enqueue(PriceLevel& lvl, Order* o) noexcept {
    o->prev_at_level = lvl.tail;
    o->next_at_level = nullptr;
    if (lvl.tail) lvl.tail->next_at_level = o;
    else          lvl.head = o;
    lvl.tail = o;
    lvl.total_quantity += o->quantity;
    ++lvl.order_count;
}
```

---

### 2.4 The Price Level Index — The Heart of the Book

This is where most design complexity lives. You need to:

1. Find the **best bid** (max price) and **best ask** (min price) in O(1).
2. **Insert** a new price level in O(1) amortized.
3. **Remove** an empty price level in O(1).
4. **Iterate** adjacent price levels for sweep matching.

#### Option A: The `std::map` Naïve Approach — Do Not Use

```cpp
std::map<int64_t, PriceLevel> bids; // Red-black tree — heap allocations, pointer chasing, cache misses
```

`std::map` is catastrophic for HFT. Every operation touches heap-allocated nodes scattered across RAM.

#### Option B: The Sorted Array with a Cache (Best for Narrow Spreads)

For instruments where prices cluster in a narrow range (equities on a major exchange), you can use a **flat circular array** indexed by price offset from a base price:

```cpp
template<int64_t BasePrice, std::size_t NumTicks, int64_t TickSize>
class LevelArray {
    // Maps price -> PriceLevel index
    // Array index = (price - BasePrice) / TickSize
    alignas(64) PriceLevel levels_[NumTicks];
    // Doubly-ended priority tracking
    int64_t best_bid_price_ = INT64_MIN;
    int64_t best_ask_price_ = INT64_MAX;

public:
    PriceLevel* get(int64_t price) noexcept {
        const std::size_t idx = (price - BasePrice) / TickSize;
        return (idx < NumTicks) ? &levels_[idx] : nullptr;
    }
};
```

- O(1) lookup, insert, and delete — just array indexing.
- Best bid/ask tracked as separate integers, updated on insert/cancel.
- **Cache behavior**: For a 1-cent tick size and $10 range, `NumTicks = 1000`, meaning all 1000 price levels fit in ~64KB — easily in L1/L2.
- The CPU prefetcher handles sequential sweep matching beautifully.

**Resizing**: If price drifts outside the array range, you re-center. This is a rare cold-path operation.

#### Option C: `ska::flat_hash_map` + Sorted Intrusive Tree (Hybrid)

For wide-spread instruments (options, FX crosses), use:

- A **Robin Hood hash map** (`int64_t price → PriceLevel*`) for O(1) lookup.
- An **intrusive red-black tree** (e.g., Boost.Intrusive `rbtree`) for ordered traversal, where tree nodes are embedded directly inside `PriceLevel`.

```cpp
#include <boost/intrusive/set.hpp>

struct PriceLevel : public boost::intrusive::set_base_hook<> {
    int64_t price;
    // ... rest of level fields
    bool operator<(const PriceLevel& o) const { return price < o.price; }
};

boost::intrusive::set<PriceLevel> bid_tree_; // No heap allocation for tree nodes!
```

Boost.Intrusive stores the tree hooks *inside* the `PriceLevel` struct itself — there are no separately allocated tree nodes. The tree is a view over your existing objects.

---

### 2.5 The Order ID Map

Cancel and modify operations arrive as `(order_id, new_quantity)`. You need O(1) lookup from order ID to Order object:

```cpp
// A flat open-addressing hash map, keys are order IDs (monotonic uint64_t)
// Implemented as a power-of-two array with linear probing
class OrderMap {
    struct Slot {
        uint64_t key;    // order_id; 0 = empty
        Order*   value;
    };
    alignas(64) Slot table_[1 << 22]; // 4M slots, 64MB — fits in LLC
    // ...
};
```

Since order IDs are monotonically increasing integers, you can also use a **direct-mapped array** (`Order* map[MAX_ORDER_ID]`), which is a single array dereference with perfect cache behavior — if the working set fits.

---

## 3. The Matching Algorithm

The matching loop is the innermost hot loop. It runs on every new aggressive order:

```cpp
void match(Order* aggressor, LevelArray& book, OrderPool& pool) noexcept {
    // Determine the passive side
    auto& passive_best = (aggressor->side == BID)
        ? book.best_ask_level()
        : book.best_bid_level();

    while (aggressor->quantity > 0 && passive_best != nullptr) {
        // Price check
        if (aggressor->side == BID && aggressor->price < passive_best->price) break;
        if (aggressor->side == ASK && aggressor->price > passive_best->price) break;

        Order* passive = passive_best->head; // FIFO — always the oldest order

        const uint32_t fill_qty = std::min(aggressor->quantity, passive->quantity);
        const int64_t  fill_px  = passive->price; // passive price wins

        // Emit trade event (lock-free SPSC queue to risk/clearing)
        emit_trade(aggressor->order_id, passive->order_id, fill_px, fill_qty);

        aggressor->quantity -= fill_qty;
        passive->quantity   -= fill_qty;
        passive_best->total_quantity -= fill_qty;

        if (passive->quantity == 0) {
            // Remove filled passive order — O(1) intrusive list removal
            dequeue(passive_best, passive);
            pool.release(passive);
            if (passive_best->order_count == 0) {
                book.remove_level(passive_best->price);
                passive_best = book.next_best_level(aggressor->side ^ 1);
            }
        }
    }

    // If aggressor has remaining quantity and is a resting order type, add to book
    if (aggressor->quantity > 0 && aggressor->type == LIMIT) {
        book.insert_or_get(aggressor->price)->enqueue(aggressor);
    } else {
        pool.release(aggressor); // IOC/FOK: discard remainder
    }
}
```

Key properties:
- **Zero branches** on the fill path that touch memory speculatively.
- The `emit_trade()` call writes to a **lock-free SPSC ring buffer** — nanoseconds, not microseconds.
- No virtual dispatch anywhere in this loop.

---

## 4. Memory Layout and Cache Strategy

### 4.1 Hot/Cold Splitting

Split `Order` into hot fields (read every match) and cold fields (needed only on cancel/ack):

```cpp
struct OrderHot {  // 64 bytes, lives in L1 during matching
    int64_t  price;
    uint32_t quantity;
    uint8_t  side;
    Order*   next_at_level;
    Order*   prev_at_level;
    // ...
};

struct OrderCold { // Separate cache line, rarely touched
    uint64_t  timestamp_ns;
    uint32_t  trader_id;
    char      client_order_id[20];
    // ...
};

struct Order {
    OrderHot  hot;
    OrderCold cold;
};
```

### 4.2 False Sharing Prevention

All shared structures between threads (if you have a multi-threaded architecture) are padded to cache line boundaries with `alignas(64)` and explicit padding arrays to prevent two independent variables from living on the same cache line.

### 4.3 NUMA Awareness

On multi-socket servers, all matching engine memory (order pool, level arrays) is allocated on the **local NUMA node** of the matching thread using `mbind()` / `numa_alloc_onnode()`. A pointer that crosses a NUMA domain adds ~100ns of latency.

---

## 5. Lock-Free Event Queues

The matching engine sits between two SPSC (Single Producer, Single Consumer) ring buffers:

```
[Network Thread] --[Inbound SPSC]--> [Matching Thread] --[Outbound SPSC]--> [Risk/Gateway Thread]
```

```cpp
template<typename T, std::size_t Size>
class SPSCQueue {
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
    alignas(64) std::atomic<uint64_t> write_idx_{0};
    alignas(64) std::atomic<uint64_t> read_idx_{0};
    alignas(64) T buffer_[Size];

public:
    bool push(const T& item) noexcept {
        const uint64_t w = write_idx_.load(std::memory_order_relaxed);
        const uint64_t r = read_idx_.load(std::memory_order_acquire);
        if (w - r == Size) return false; // full
        buffer_[w & (Size - 1)] = item;
        write_idx_.store(w + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& item) noexcept {
        const uint64_t r = read_idx_.load(std::memory_order_relaxed);
        const uint64_t w = write_idx_.load(std::memory_order_acquire);
        if (r == w) return false; // empty
        item = buffer_[r & (Size - 1)];
        read_idx_.store(r + 1, std::memory_order_release);
        return true;
    }
};
```

- One atomic load per operation with `acquire`/`release` semantics — no `mfence` (which stalls the entire store buffer).
- The producer and consumer cache lines are on **separate cache lines** (the two `alignas(64)` attributes) — this is critical. Without this, every `push` invalidates the consumer's cache line.

---

## 6. Timestamp and Clock

Never use `std::chrono::system_clock` or `clock_gettime(CLOCK_REALTIME)` in the hot path. These invoke a syscall on non-VDSO kernels.

Use the **TSC (Time Stamp Counter)** directly:

```cpp
inline uint64_t rdtsc() noexcept {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// For ordering guarantees (prevents out-of-order execution across the fence):
inline uint64_t rdtscp() noexcept {
    uint32_t lo, hi, aux;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
```

Convert to nanoseconds once, using a pre-calibrated `tsc_to_ns` multiplier computed at startup via `clock_gettime(CLOCK_MONOTONIC_RAW)`.

---

## 7. Summary of Data Structure Choices

| Component | Structure | Reason |
|---|---|---|
| Order storage | Slab/pool allocator (flat array) | Zero heap allocation on hot path |
| Order queue per level | Intrusive doubly-linked list | O(1) enqueue/dequeue, zero allocation |
| Price level index (narrow spread) | Flat array indexed by price offset | O(1) all ops, prefetcher-friendly |
| Price level index (wide spread) | Intrusive RB-tree + Robin Hood map | O(log N) ordered, O(1) lookup |
| Order ID lookup | Direct-mapped array or open-addressing hash map | O(1) cancel/modify |
| Inter-thread communication | SPSC lock-free ring buffer | No contention, no locks |
| Timestamps | Raw TSC via `rdtsc`/`rdtscp` | No syscall overhead |

The overarching principle is **structure of arrays over array of structures where access patterns demand it**, relentless **cache line alignment**, **zero dynamic allocation** in the critical path, and treating every pointer dereference as a potential multi-hundred-cycle stall to be eliminated.

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Describe the structure of the matching engine in the high-frequency trading system in depth. Describe the data structures used in the C++ implementation of the matching engine to ensure low latency and jitter. This description is intended for a computer science expert who is willing to implement this piece of code.
