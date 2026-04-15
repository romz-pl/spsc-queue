# Design Notes and Trade-Offs


`SPSCQueue<T, N>` is a **Single-Producer / Single-Consumer (SPSC) lock-free ring buffer** implemented in C++11 and later. It is designed for scenarios where exactly one thread enqueues items and exactly one (different) thread dequeues them, with the primary goals of **zero locking overhead**, **minimal cache contention**, and **predictable latency**.

---

## 1. Data Structure Layout

```
 index:  0     1     2   ...   N-1
        ┌─────┬─────┬─────┬─────┐
buffer  │  T  │  T  │  T  │  T  │
        └─────┴─────┴─────┴─────┘
            ▲                 ▲
           head              tail
      (producer cursor)  (consumer cursor)
```

| Member | Owner | Role |
|--------|-------|------|
| `head` | Producer writes, consumer reads | Points to the **next slot to be written** |
| `tail` | Consumer writes, producer reads | Points to the **next slot to be read** |

The buffer is treated as a **circular array** using modulo arithmetic (`% N`).

---

## 2. Capacity vs. Usable Slots

> **The buffer holds at most `N − 1` items at any time.**

One slot is intentionally left empty so that the full condition (`head + 1 == tail`) is distinguishable from the empty condition (`head == tail`) without a separate counter. This is the classic *sacrificed slot* trade-off:

- **Pro:** No extra synchronisation needed for a count variable.
- **Con:** Effective capacity is `N − 1`, which must be accounted for at call sites.

The `static_assert(N >= 2, ...)` guard prevents degenerate zero-capacity configurations.

---

## 3. Memory Ordering — The Core Design Decision

The implementation uses the **minimum necessary memory-ordering constraints**, deliberately avoiding the heavier `memory_order_seq_cst` default:

### 3.1 `push()`

```cpp
head.load(std::memory_order_relaxed)   // ① own cursor — no sync needed
tail.load(std::memory_order_acquire)   // ② read consumer's progress → pairs with ④
buffer[h] = val;                       // ③ write payload before publishing index
head.store(next, std::memory_order_release) // ④ publish to consumer → pairs with ②
```

### 3.2 `pop()`

```cpp
tail.load(std::memory_order_relaxed)   // ① own cursor — no sync needed
head.load(std::memory_order_acquire)   // ② read producer's progress → pairs with ④
val = buffer[t];                       // ③ read payload after observing new index
tail.store((t+1)%N, std::memory_order_release) // ④ publish to producer → pairs with ②
```

### Why this pairing is correct

The **acquire/release pair on the shared cursor** creates a *happens-before* edge:

```
producer:  buffer[h] = val  →  head.store(release)
                                         ↕  synchronises-with
consumer:                      head.load(acquire)  →  val = buffer[t]
```

This guarantees the payload write is visible before the payload read, without a full memory fence. On weakly-ordered architectures (ARM, POWER) this generates only the instructions actually required; on x86, `release`/`acquire` on stores/loads compile to plain `MOV` instructions with no extra fences.

### Trade-off summary

| Approach | Overhead | Correctness |
|---|---|---|
| `seq_cst` everywhere | Higher (full fence on x86 stores) | ✓ |
| `acquire`/`release` (chosen) | Minimal | ✓ |
| `relaxed` everywhere | Lowest | ✗ (data race) |

---

## 4. False Sharing Considerations

Both atomic cursors (`head`, `tail`) and the payload `buffer` reside in the same struct. On a typical 64-byte cache line this creates a **potential false-sharing problem**: the producer's writes to `head` may invalidate the cache line holding `tail` in the consumer's L1 cache.

### Current design choice (not padded)

The implementation omits explicit cache-line padding to keep the code simple and portable. This is acceptable when:

- `sizeof(T) * N` is large (buffer dominates cache traffic).
- Throughput, rather than latency per item, is the primary concern.

### Alternative: padded cursors

For ultra-low-latency use cases, the struct can be modified to separate the two hot atomic variables onto distinct cache lines:

```cpp
alignas(64) std::atomic<size_t> head{0};
alignas(64) std::atomic<size_t> tail{0};
```

**Trade-off:** ~64 bytes of extra padding per queue instance; significantly reduced cache-line ping-pong on multi-socket systems.

---

## 5. Power-of-Two vs. Arbitrary Capacity

The modulo operation `(h + 1) % N` is used for wrapping.

- **Arbitrary N (current design):** Simple and flexible. The compiler may or may not optimise the division away.
- **Power-of-two N:** Wrapping becomes a bitmask `(h + 1) & (N - 1)`, which is a single instruction. This can be enforced with an additional `static_assert`:

```cpp
static_assert((N & (N - 1)) == 0, "N must be a power of two");
```

**Trade-off:** Power-of-two constraint is a usability restriction, but can yield measurable throughput gains in tight loops on some microarchitectures.

---

## 6. Blocking Spin Wrappers

`push_blocking()` and `pop_blocking()` implement a **busy-wait spin** with `std::this_thread::yield()` on failure.

### Design intent

| Behaviour | Reason |
|---|---|
| `yield()` instead of `sleep_for()` | Minimises latency when the other thread makes progress quickly |
| No backoff strategy | Keeps the implementation simple; assumes short wait times |

### Trade-offs

- **Pro:** Near-zero wakeup latency when the queue becomes available within a few microseconds.
- **Con:** Burns CPU cycles if the producer and consumer run at very different rates. In that scenario, an **exponential backoff** or a **futex / condition variable** would be more appropriate.
- **Con:** `yield()` is a hint to the OS scheduler; on a lightly-loaded system it may immediately return to the same thread, making it effectively a busy loop.

---

## 7. SPSC Constraint — What Breaks with Multiple Producers/Consumers

The safety guarantees rely on **exactly one writer to `head`** and **exactly one writer to `tail`** at all times. Violating this causes undefined behaviour (concurrent unsynchronised writes to the same atomic without exclusive ownership):

| Violation | Failure mode |
|---|---|
| Two producers call `push()` concurrently | Both read the same `h`, both write to `buffer[h]`, one item lost |
| Two consumers call `pop()` concurrently | Both read the same `t`, same item delivered twice |

For MPSC / MPMC scenarios a different algorithm (e.g., a lock-based queue or a CAS-based MPMC ring) is required.

---

## 8. Exception Safety

The current implementation is **not exception-safe** with respect to the payload type `T`:

- `buffer[h] = val` invokes `T::operator=`. If that throws, `head` has not yet been advanced, so the queue remains consistent — but the partially-written slot contains an indeterminate value.
- `val = buffer[t]` invokes `T::operator=`. If that throws, `tail` has not yet been advanced, leaving the item logically un-consumed.

**Trade-off:** Supporting strong exception guarantees would require storing items in uninitialized storage (`std::aligned_storage`) and using placement new/explicit destruction, significantly increasing code complexity.

---

## 9. Summary of Key Trade-Offs

| Design choice | Benefit | Cost |
|---|---|---|
| Lock-free SPSC | Zero mutex overhead, cache-friendly | Only 1 producer + 1 consumer |
| Sacrificed slot for full/empty | No extra counter atomic | Usable capacity is `N − 1` |
| `acquire`/`release` ordering | Minimum fence instructions | More subtle correctness argument |
| No cache-line padding | Simpler code, smaller struct | Potential false sharing at high throughput |
| Arbitrary N (modulo) | Flexible capacity | Possible division cost vs. bitmask |
| `yield()`-based spin | Low wakeup latency | CPU waste under sustained imbalance |
| Copy-based payload | Simple value semantics | No zero-copy / move support in current form |

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Generate Markdown-format documentation related to "Detailed Design Notes and Trade-Offs" for the following code: [spsc_queue.hpp]
> 
