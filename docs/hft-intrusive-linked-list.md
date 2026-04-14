# Intrusive Linked List for an HFT Order Queue

## Core Concept

An **intrusive linked list** embeds the list node (the `prev`/`next` pointers) directly inside the payload struct, rather than allocating a separate wrapper node that holds a pointer to the payload. In a non-intrusive list you'd have:

```
ListNode { next*, prev*, void* data }  →  Order { price, qty, ... }
```

In an intrusive list:

```
Order { price, qty, ..., next*, prev* }
```

This seemingly small distinction has profound consequences for cache behavior, allocation patterns, and latency — all critical in HFT.

---

## Why Intrusive for an Order Queue?

| Concern | Non-Intrusive | Intrusive |
|---|---|---|
| Cache lines on traversal | 2 pointer chases per node (wrapper → payload) | 1 — node *is* the payload |
| Heap allocations per order | 2 (wrapper + payload) | 1 (payload only) |
| Allocator lock contention | Doubled | Halved |
| Object membership test | O(n) or needs a set | O(1) via `container_of` |
| Multiple list membership | Requires multiple wrappers | Multiple hook pairs in the struct |

In a market with microsecond-level order lifecycle (insert → match → cancel), every avoided allocation and every saved cache miss directly translates to P&L.

---

## Data Structure Layout

```c
/* The intrusive hook — embedded inside the order */
typedef struct list_hook {
    struct list_hook *next;
    struct list_hook *prev;
} list_hook_t;

/* The order struct */
typedef struct order {
    /* ── Hot fields (cache line 0) ── */
    uint64_t     order_id;
    int64_t      price;          /* Fixed-point, e.g. price * 1e8 */
    uint32_t     qty;
    uint32_t     filled_qty;
    uint8_t      side;           /* BID=0 / ASK=1 */
    uint8_t      type;           /* LIMIT, MARKET, IOC, FOK */
    uint8_t      status;
    uint8_t      _pad0;

    /* ── List hooks (cache line 0 or 1) ── */
    list_hook_t  price_level_hook;   /* membership in price-level FIFO */
    list_hook_t  client_hook;        /* membership in per-client order list */

    /* ── Cold fields (cache line 1+) ── */
    uint64_t     timestamp_ns;
    uint64_t     expiry_ns;
    uint32_t     client_id;
    uint32_t     instrument_id;
    char         client_order_ref[16];
} order_t;

/* Sentinel-based doubly-linked list head */
typedef struct {
    list_hook_t  sentinel;    /* sentinel.next = first, sentinel.prev = last */
    size_t       count;
} order_list_t;
```

**Cache line discipline** is paramount. The hot fields and both hooks should ideally fit within the first 64 bytes (one cache line) so that a prefetch of the order for matching also brings in the list pointers.

---

## The `container_of` Macro

This is the central mechanism that makes intrusive lists work. Given a pointer to a `list_hook_t` embedded inside an `order_t`, recover a pointer to the enclosing `order_t`:

```c
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* Usage */
list_hook_t *hook = sentinel.next;
order_t     *order = container_of(hook, order_t, price_level_hook);
```

This is a **zero-cost abstraction** — it compiles to a single `SUB` instruction (subtract a compile-time constant offset). No indirection, no branch, no allocation.

---

## Sentinel-Based Design (Circular)

Use a **circular doubly-linked list with a sentinel node** (also called a "list head" or "anchor"). The sentinel is a `list_hook_t` that is always present and never corresponds to an order. This eliminates all `NULL` checks on `next`/`prev` during traversal and mutation:

```
sentinel ⇄ order_A ⇄ order_B ⇄ order_C ⇄ (back to sentinel)
```

- `sentinel.next` → first real order (or `sentinel` if empty)
- `sentinel.prev` → last real order (or `sentinel` if empty)
- `list_is_empty(list)` ≡ `list->sentinel.next == &list->sentinel`

```c
static inline void list_init(order_list_t *list) {
    list->sentinel.next = &list->sentinel;
    list->sentinel.prev = &list->sentinel;
    list->count = 0;
}

static inline int list_is_empty(const order_list_t *list) {
    return list->sentinel.next == &list->sentinel;
}
```

---

## Core Operations

All operations are **O(1)** and branch-free (no NULL checks due to the sentinel).

### Insert at Tail (FIFO enqueue — time priority)

```c
static inline void list_push_back(order_list_t *list, list_hook_t *hook) {
    list_hook_t *prev = list->sentinel.prev;   /* current last node */
    hook->next        = &list->sentinel;
    hook->prev        = prev;
    prev->next        = hook;
    list->sentinel.prev = hook;
    list->count++;
}
```

### Insert at Head

```c
static inline void list_push_front(order_list_t *list, list_hook_t *hook) {
    list_hook_t *next = list->sentinel.next;
    hook->prev        = &list->sentinel;
    hook->next        = next;
    next->prev        = hook;
    list->sentinel.next = hook;
    list->count++;
}
```

### Remove Arbitrary Node (O(1) — the killer feature)

This is the operation that most justifies the intrusive design. Cancelling an order requires removing it from a price-level queue without searching:

```c
static inline void list_remove(order_list_t *list, list_hook_t *hook) {
    hook->prev->next = hook->next;
    hook->next->prev = hook->prev;
    /* Poison the pointers to catch use-after-free in debug builds */
#ifdef DEBUG
    hook->next = (list_hook_t *)0xDEADDEADDEADDEADULL;
    hook->prev = (list_hook_t *)0xDEADDEADDEADDEADULL;
#endif
    list->count--;
}
```

In a traditional queue, cancellation requires either a linear search or a side-table (e.g., `unordered_map<order_id, node*>`). With an intrusive list, the `order_t` *itself* is the node — the cancel handler receives an `order_t*`, computes `&order->price_level_hook`, and calls `list_remove` directly. **No lookup needed.**

### Dequeue from Front (matching engine — best price FIFO)

```c
static inline order_t *list_pop_front(order_list_t *list) {
    if (list_is_empty(list)) return NULL;
    list_hook_t *hook = list->sentinel.next;
    list_remove(list, hook);
    return container_of(hook, order_t, price_level_hook);
}
```

### Traversal

```c
#define LIST_FOR_EACH(list, cursor)                              \
    for ((cursor) = (list)->sentinel.next;                       \
         (cursor) != &(list)->sentinel;                          \
         (cursor) = (cursor)->next)

/* Usage */
list_hook_t *cur;
LIST_FOR_EACH(&price_level->orders, cur) {
    order_t *o = container_of(cur, order_t, price_level_hook);
    /* process o */
}

/* Safe variant for removal during traversal */
#define LIST_FOR_EACH_SAFE(list, cursor, tmp)                    \
    for ((cursor) = (list)->sentinel.next,                       \
         (tmp)    = (cursor)->next;                              \
         (cursor) != &(list)->sentinel;                          \
         (cursor) = (tmp), (tmp) = (tmp)->next)
```

---

## Memory Management Strategy

HFT systems never use `malloc`/`free` on the hot path. Orders are managed via a **lock-free slab/pool allocator**:

```c
typedef struct {
    order_t      *pool;          /* contiguous array of orders */
    list_hook_t   free_list;     /* intrusive free list reusing price_level_hook */
    size_t        capacity;
    _Atomic size_t free_count;
} order_pool_t;

order_t *pool_alloc(order_pool_t *pool) {
    if (list_is_empty_raw(&pool->free_list)) return NULL;
    list_hook_t *hook = pool->free_list.next;
    /* remove from free list using raw hook manipulation */
    hook->prev->next = hook->next;
    hook->next->prev = hook->prev;
    return container_of(hook, order_t, price_level_hook);
}

void pool_free(order_pool_t *pool, order_t *o) {
    /* re-init the hook and push to front of free list */
    list_push_front_raw(&pool->free_list, &o->price_level_hook);
}
```

Key insight: the **free list itself is an intrusive list** that reuses the same `price_level_hook` field. When an order is in the free pool, that hook links it in the pool's free list. When live, the hook links it in a price-level queue. The field serves double duty at zero cost.

---

## Multiple List Membership

The dual hooks (`price_level_hook` and `client_hook`) allow a single `order_t` to be simultaneously a member of:

1. **A price-level FIFO queue** — for matching engine time-priority within a price level
2. **A per-client order list** — for client-level operations (risk checks, mass cancel / "nuke" on disconnect)

This is impossible without either code duplication or heap overhead in a non-intrusive design. With intrusive lists, you simply embed as many hooks as you need, and membership in each list is independent.

---

## Concurrency Considerations

In a single-threaded matching engine (the canonical HFT architecture — one thread per instrument, pinned to a core), the list requires **no locks**. This is the preferred design.

If multi-threaded access is required (e.g., a gateway thread inserting while the engine thread matches), protect at the price-level granularity with a **seqlock** or **RCU**, not a mutex:

- **Seqlock**: writer increments a sequence counter before and after mutation; reader retries if it observes an odd counter (write in progress). Zero reader overhead on the uncontended fast path.
- **RCU (Read-Copy-Update)**: readers are never blocked; writers publish a new version atomically and defer reclamation. Excellent when reads dominate (order book queries >> order mutations).

Avoid `std::mutex` — it may invoke a futex syscall and yield the CPU, which is catastrophic for a latency-sensitive thread.

---

## Assembly-Level Perspective

The `list_remove` operation on x86-64 with `-O2` compiles to approximately:

```asm
; rdi = hook pointer
mov rax, [rdi + 8]      ; rax = hook->next
mov rcx, [rdi]          ; rcx = hook->prev
mov [rcx + 8], rax      ; hook->prev->next = hook->next
mov [rax], rcx          ; hook->next->prev = hook->prev
dec [list_count]        ; list->count--
ret
```

That is **5 instructions**, all operating on data already in L1 cache if the surrounding order was recently touched. This is the floor for pointer-based list removal. There is no faster general-purpose linked-list deletion.

---

## Summary of Design Invariants

1. **The sentinel is never `NULL`** — eliminating all conditional branches on `next`/`prev`.
2. **`container_of` is the only way to go from hook → order** — this enforces a clean API boundary.
3. **Pool allocation only on the hot path** — `malloc` is banned during market hours.
4. **Pointer poisoning in debug builds** — catches use-after-free and double-remove immediately.
5. **Cache-line alignment of `order_t`** — use `alignas(64)` to prevent false sharing between adjacent orders in the pool array.
6. **Each hook belongs to exactly one list at a time** — enforced by convention; the pool's free list and the price-level list are mutually exclusive states for `price_level_hook`.

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Describe the intrusive linked list for the order queue in the high-frequency trading system in depth. This description is intended for a computer science expert who is willing to implement this piece of code.
