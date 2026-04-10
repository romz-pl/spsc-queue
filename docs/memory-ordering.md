# Memory Ordering in SPSC Lock-Free Ring Buffers

## The Core Problem: Compiler & CPU Reordering

Modern compilers and CPUs aggressively reorder instructions for performance. In a single thread this is invisible, but across threads it causes races. `acquire`/`release` are the surgical tool that prevents exactly the reorderings that break a ring buffer.

---

## What `release` and `acquire` actually guarantee

**`release`** (used by the **producer** when publishing `head`):
> *"Every store I did before this store is visible to any thread that subsequently acquires this value."*

**`acquire`** (used by the **consumer** when reading `head`):
> *"Every load I do after this load will see all stores that happened before the matching release."*

Together they form a **synchronizes-with** relationship — a causal happens-before edge across threads.

---

## A Minimal SPSC Buffer

```cpp
template<typename T, size_t N>
struct SPSCQueue {
    T buffer[N];
    std::atomic<size_t> head{0};  // written by producer, read by consumer
    std::atomic<size_t> tail{0};  // written by consumer, read by producer

    // --- PRODUCER ---
    bool push(const T& val) {
        size_t h = head.load(std::memory_order_relaxed); // only we write head
        size_t next = (h + 1) % N;

        if (next == tail.load(std::memory_order_acquire)) // (1)
            return false; // full

        buffer[h] = val;                                  // (2) write payload
        head.store(next, std::memory_order_release);      // (3) publish
        return true;
    }

    // --- CONSUMER ---
    bool pop(T& val) {
        size_t t = tail.load(std::memory_order_relaxed); // only we write tail
        if (t == head.load(std::memory_order_acquire))   // (4)
            return false; // empty

        val = buffer[t];                                  // (5) read payload
        tail.store((t + 1) % N, std::memory_order_release); // (6) free slot
        return true;
    }
};
```

---

## Why each ordering is necessary

### Producer side — `release` at (3)

The payload write (2) **must be visible before** the index update (3) is seen by the consumer.

```
Producer thread           Consumer thread
──────────────────        ──────────────────
buffer[h] = val    (2)
                          head.load(acquire) (4)  ← sees next
                          val = buffer[t]    (5)  ← MUST see val
head.store(release) (3)
```

Without `release`, the CPU (e.g. x86's store-buffer, or ARM's weak model) is free to reorder (3) before (2) — the consumer could observe the updated `head` index but read **stale/garbage data** from `buffer[h]`.

### Consumer side — `acquire` at (4)

Without `acquire` on the `head` load, the compiler or CPU can **speculate / hoist** the `buffer[t]` load (5) *before* confirming that `head` has advanced. You'd read from a slot the producer hasn't finished writing yet.

The `acquire` acts as a **one-way fence**: no loads/stores after it can be moved above it.

### Producer reading `tail` — `acquire` at (1)

The consumer's `release` on `tail` (6) pairs with this. It ensures the producer sees that the slot has truly been **consumed** before overwriting it. Without it, the producer could wrap around and clobber a slot the consumer is still reading.

---

## What goes wrong with `relaxed` everywhere

| Scenario | Failure mode |
|---|---|
| Producer: `head` store relaxed | Consumer sees new index, loads **garbage** from buffer (write reordered after index publish) |
| Consumer: `head` load relaxed | Consumer speculatively reads buffer **before** the slot is ready |
| Producer: `tail` load relaxed | Producer wraps around and **overwrites a slot** the consumer hasn't finished reading |
| Consumer: `tail` store relaxed | Producer re-uses a slot while consumer is **mid-read** |

All of these are **data races** — undefined behavior in C++, and real corruption on weakly-ordered architectures (ARM, POWER, RISC-V).

---

## A note on x86

On x86, the hardware memory model is **TSO (Total Store Order)** — stores are never reordered with other stores, and loads are never reordered with other loads. So `relaxed` *often works in practice* on x86, which is exactly why these bugs are so insidious: they **hide on x86 and explode on ARM**. The `acquire`/`release` pairs compile to plain `MOV` on x86 anyway (zero extra cost), so there's no reason not to use them.

---

## The one-sentence summary

`release` ensures the **payload is written before the index is published**; `acquire` ensures the **index is confirmed before the payload is read** — without both, you have a window where one thread acts on a pointer into memory the other thread hasn't finished touching.
