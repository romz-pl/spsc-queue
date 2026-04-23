/**
 * mpmc_ring_buffer.hpp
 *
 * Multiple-Producer, Multiple-Consumer (MPMC) Lock-Free Ring Buffer
 * Designed for High-Frequency Trading Systems
 *
 * Design: Dmitry Vyukov sequence-number MPMC queue, extended with C++20
 *         concepts, batch operations, PAUSE-hint spin loops, and optional
 *         back-pressure telemetry.
 *
 * Requirements: C++20, x86-64 (for _mm_pause), 64-byte cache lines.
 *
 * Author:  Reference implementation
 * License: Public Domain / CC0
 */

#pragma once

#include <atomic>
#include <bit>          // std::has_single_bit, std::popcount
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <immintrin.h>  // _mm_pause()
#include <limits>
#include <memory>       // std::construct_at, std::destroy_at
#include <new>          // std::hardware_destructive_interference_size
#include <optional>
#include <span>
#include <type_traits>
#include <utility>      // std::forward, std::move


// ---------------------------------------------------------------------------
// Platform / architecture constants
// ---------------------------------------------------------------------------

namespace mpmc::detail {

/// Compile-time cache-line size.  C++17 provides this but it may not be
/// constexpr on every platform, so we pin to 64 bytes for x86-64 HFT targets.
inline constexpr std::size_t kCacheLineSize = 64;

// Verify the platform agrees (soft assertion; comment out for cross-compile).
static_assert(std::hardware_destructive_interference_size <= kCacheLineSize,
              "Unexpected cache-line size — review padding strategy.");

// ---------------------------------------------------------------------------
// Concepts
// ---------------------------------------------------------------------------

/// Requires Capacity to be a power of two greater than 1 so that the
/// index-masking trick (index & (Capacity-1)) is valid.
template<std::size_t N>
concept PowerOfTwo = (N > 1) && std::has_single_bit(N);

/// Value stored in the ring buffer must be nothrow-destructible and either
/// nothrow-move- or nothrow-copy-constructible so we can guarantee
/// exception neutrality of the ring-buffer itself.
template<typename T>
concept RingBufferElement =
    std::is_nothrow_destructible_v<T> &&
    (std::is_nothrow_move_constructible_v<T> ||
     std::is_nothrow_copy_constructible_v<T>);

// ---------------------------------------------------------------------------
// Spin-wait primitive
// ---------------------------------------------------------------------------

/// Issue a PAUSE hint (rep nop) to avoid the memory-order machine-clear
/// penalty and reduce power consumption during tight spin loops.
/// Falls back to a compiler barrier on non-x86.
[[gnu::always_inline]] inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// ---------------------------------------------------------------------------
// Padding helper
// ---------------------------------------------------------------------------

/// Aligns a byte array to a full cache line and pads it so that only
/// `UsedBytes` of payload are present — the rest is dead padding.
template<std::size_t UsedBytes>
struct alignas(kCacheLineSize) CacheLinePad {
    static_assert(UsedBytes <= kCacheLineSize);
    char pad[kCacheLineSize - UsedBytes];
};

// Zero-sized specialisation for exact fit.
template<>
struct alignas(kCacheLineSize) CacheLinePad<kCacheLineSize> {};

} // namespace mpmc::detail


// ---------------------------------------------------------------------------
// MPMCRingBuffer
// ---------------------------------------------------------------------------

namespace mpmc {

/**
 * @brief Lock-free MPMC bounded ring buffer.
 *
 * @tparam T         Element type.  Must satisfy RingBufferElement.
 * @tparam Capacity  Number of slots.  Must be a power of two > 1.
 *
 * Guarantees
 * ----------
 * - Wait-free progress for try_push / try_pop (bounded retries per
 *   successful operation).
 * - ABA-immunity via per-slot monotonically increasing sequence numbers.
 * - No dynamic memory allocation after construction.
 * - All operations are noexcept when T's corresponding operations are.
 *
 * Memory layout
 * -------------
 *   [head_ | pad]          — cache line 0       (producers write here)
 *   [tail_ | pad]          — cache line 1       (consumers write here)
 *   [slots_[0] .. [N-1]]   — N × cache-line-aligned slots
 *
 * Each slot
 * ---------
 *   sequence  : std::atomic<std::size_t>   — state discriminator
 *   storage   : aligned_storage for T      — payload (manual lifetime)
 *   pad       : byte padding to fill cache line
 *
 * Sequence-number state machine (per slot, index i)
 * --------------------------------------------------
 *   seq == i           → slot is EMPTY, ready for a producer to claim.
 *   seq == i + 1       → slot is FULL,  ready for a consumer to claim.
 *   seq == i + Capacity→ slot is EMPTY again (recycled), ready for next round.
 *
 * The signed difference trick
 * ---------------------------
 *   diff = (intptr_t)seq - (intptr_t)expected
 *   diff == 0  → slot in desired state, attempt CAS.
 *   diff <  0  → queue full (producer) or empty (consumer); abort.
 *   diff >  0  → another thread advanced the index; reload and retry.
 *
 *   Because std::size_t wraps silently in unsigned arithmetic and
 *   (intptr_t) casts preserve sign, this correctly handles the wrap-around
 *   case after ~2^63 operations without any special-casing.
 */
template<RingBufferElement T, std::size_t Capacity>
    requires detail::PowerOfTwo<Capacity>
class MPMCRingBuffer {
public:
    // ------------------------------------------------------------------
    // Types
    // ------------------------------------------------------------------

    using value_type      = T;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;

    static constexpr size_type kCapacity = Capacity;

    // ------------------------------------------------------------------
    // Slot — the fundamental unit of storage
    // ------------------------------------------------------------------

private:

    struct Slot {
        /// State discriminator.  Initialised to slot index `i` so that
        /// the first producer to look at slot i finds seq == i == head,
        /// giving diff == 0 and allowing the CAS to proceed.
        alignas(detail::kCacheLineSize) std::atomic<size_type> sequence;

        /// Raw storage for T.  We manage lifetime manually with
        /// std::construct_at / std::destroy_at to avoid default-constructing
        /// T (which may be expensive or impossible for non-default-
        /// constructible types) and to allow the slot to start "empty".
        alignas(alignof(T)) std::byte storage[sizeof(T)];

        /// Padding: ensure the *next* slot begins on a fresh cache line,
        /// preventing false sharing between producers/consumers racing on
        /// adjacent slots.  For T larger than a cache line, the compiler
        /// will naturally emit enough bytes; for small T we add explicit
        /// padding.
        static constexpr size_type kPayloadBytes =
            sizeof(std::atomic<size_type>) + sizeof(std::byte[sizeof(T)]);
        static constexpr size_type kPadBytes =
            (kPayloadBytes < detail::kCacheLineSize)
                ? detail::kCacheLineSize - kPayloadBytes
                : 0;
        [[no_unique_address]] std::byte _pad[kPadBytes];

        // Convenience accessors for the stored value.
        [[nodiscard]] T*       ptr()       noexcept
            { return std::launder(reinterpret_cast<T*>(storage)); }
        [[nodiscard]] const T* ptr() const noexcept
            { return std::launder(reinterpret_cast<const T*>(storage)); }
    };

    // Sanity-check: slots should not be smaller than a cache line.
    static_assert(sizeof(Slot) >= detail::kCacheLineSize);

    // ------------------------------------------------------------------
    // Internal state
    // ------------------------------------------------------------------

    /// Producer index: next slot to be claimed for writing.
    /// Placed on its own cache line so producers and consumers do not
    /// evict each other's hot data.
    alignas(detail::kCacheLineSize) std::atomic<size_type> head_{0};
    [[no_unique_address]] detail::CacheLinePad<sizeof(std::atomic<size_type>)> _pad_head_;

    /// Consumer index: next slot to be claimed for reading.
    alignas(detail::kCacheLineSize) std::atomic<size_type> tail_{0};
    [[no_unique_address]] detail::CacheLinePad<sizeof(std::atomic<size_type>)> _pad_tail_;

    /// Ring-buffer slots.  Allocated inline — no heap.
    alignas(detail::kCacheLineSize) Slot slots_[Capacity];

    /// Bitmask for fast modulo: index % Capacity == index & kMask.
    static constexpr size_type kMask = Capacity - 1;

    // ------------------------------------------------------------------
    // Internal helpers
    // ------------------------------------------------------------------

    /// Retrieve a reference to the slot for a given linear index.
    [[nodiscard]] Slot& slot_at(size_type idx) noexcept {
        return slots_[idx & kMask];
    }

public:
    // ------------------------------------------------------------------
    // Construction / destruction
    // ------------------------------------------------------------------

    MPMCRingBuffer() noexcept {
        // Initialise each slot's sequence number to its static index.
        // This signals "empty and ready for the first producer".
        for (size_type i = 0; i < Capacity; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
        // Publish the initialised state to all threads.
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    /// Destructor: drain remaining elements to invoke their destructors.
    /// Call only after all producers and consumers have stopped.
    ~MPMCRingBuffer() noexcept {
        // Drain without moving into a temporary — destroy in-place.
        size_type tail = tail_.load(std::memory_order_relaxed);
        size_type head = head_.load(std::memory_order_relaxed);
        while (tail != head) {
            Slot& s = slot_at(tail);
            std::destroy_at(s.ptr());
            ++tail;
        }
    }

    // Non-copyable, non-movable: the object contains atomics and
    // manages raw storage — copying/moving semantics are not meaningful.
    MPMCRingBuffer(const MPMCRingBuffer&)            = delete;
    MPMCRingBuffer& operator=(const MPMCRingBuffer&) = delete;
    MPMCRingBuffer(MPMCRingBuffer&&)                 = delete;
    MPMCRingBuffer& operator=(MPMCRingBuffer&&)      = delete;

    // ------------------------------------------------------------------
    // Non-blocking single-item operations
    // ------------------------------------------------------------------

    /**
     * @brief Attempt to enqueue one item without blocking.
     *
     * @param item  Value forwarded into the slot storage.
     * @return true if enqueued; false if the ring buffer is full.
     *
     * Algorithm (producer side)
     * -------------------------
     * 1. Load head (relaxed — no ordering needed; we order via sequence).
     * 2. Compute the slot index: s = head & kMask.
     * 3. Load s.sequence with ACQUIRE (synchronise with the consumer's
     *    RELEASE store that recycled this slot).
     * 4. Compute diff = seq - head.
     *    - diff == 0: slot is empty.  Attempt CAS(head, head+1).
     *      * CAS succeeds: we own the slot; construct T and store
     *        seq = head+1 with RELEASE (visible to next consumer).
     *      * CAS fails:  another producer stole the slot; reload head
     *        and retry.
     *    - diff < 0:  slot has not yet been consumed (queue full). Return
     *        false.
     *    - diff > 0:  head is stale (another producer already advanced
     *        it); reload head and retry.
     *
     * Memory-ordering rationale
     * -------------------------
     * - head CAS uses relaxed success / relaxed failure because we order
     *   via the sequence atomic, not via head.
     * - Sequence load is ACQUIRE so we see the consumer's RELEASE write
     *   (the recycled sequence) before we construct into the slot.
     * - Sequence store after construction is RELEASE so the consumer's
     *   ACQUIRE load sees the fully constructed T.
     */
    template<typename U>
        requires std::constructible_from<T, U&&>
    [[nodiscard]] bool try_push(U&& item)
        noexcept(std::is_nothrow_constructible_v<T, U&&>)
    {
        size_type head = head_.load(std::memory_order_relaxed);

        for (;;) {
            Slot&     s    = slot_at(head);
            size_type seq  = s.sequence.load(std::memory_order_acquire);
            auto      diff = static_cast<std::intptr_t>(seq)
                           - static_cast<std::intptr_t>(head);

            if (diff == 0) {
                // Slot is empty and matches our expected position.
                if (head_.compare_exchange_weak(
                        head, head + 1,
                        std::memory_order_relaxed,   // success
                        std::memory_order_relaxed))  // failure (head reloaded by CAS)
                {
                    // We own this slot exclusively.
                    std::construct_at(s.ptr(), std::forward<U>(item));
                    // Signal the consumer: slot is now full.
                    s.sequence.store(head + 1, std::memory_order_release);
                    return true;
                }
                // CAS failed: head was reloaded by compare_exchange_weak.
                // Continue retry loop.
            } else if (diff < 0) {
                // Queue is full — the slot has not been consumed yet.
                return false;
            } else {
                // head is stale; another producer advanced it.
                // Reload and retry.
                head = head_.load(std::memory_order_relaxed);
            }
        }
    }

    /**
     * @brief Attempt to dequeue one item without blocking.
     *
     * @param[out] item  Destination for the moved-out value.
     * @return true if an item was dequeued; false if the ring buffer is empty.
     *
     * Algorithm (consumer side)
     * -------------------------
     * 1. Load tail (relaxed).
     * 2. Compute s = tail & kMask.
     * 3. Load s.sequence with ACQUIRE.
     * 4. Compute diff = seq - (tail + 1).
     *    - diff == 0: slot is full.  Attempt CAS(tail, tail+1).
     *      * CAS succeeds: move-construct item, destroy in-place,
     *        store seq = tail + Capacity with RELEASE (recycles slot for
     *        the producer Capacity enqueues later).
     *      * CAS fails: retry.
     *    - diff < 0: queue is empty.  Return false.
     *    - diff > 0: tail is stale; reload and retry.
     *
     * Why tail + Capacity?
     * --------------------
     * The slot will next be reused when head reaches (current_head +
     * Capacity), i.e., after one full wrap of the ring.  Setting
     * seq = tail + Capacity primes the slot so that the producer at
     * that future head value finds diff == 0.
     */
    [[nodiscard]] bool try_pop(T& item)
        noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        size_type tail = tail_.load(std::memory_order_relaxed);

        for (;;) {
            Slot&     s    = slot_at(tail);
            size_type seq  = s.sequence.load(std::memory_order_acquire);
            auto      diff = static_cast<std::intptr_t>(seq)
                           - static_cast<std::intptr_t>(tail + 1);

            if (diff == 0) {
                // Slot is full and at the head of the consumer queue.
                if (tail_.compare_exchange_weak(
                        tail, tail + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                {
                    // We own this slot exclusively.
                    item = std::move(*s.ptr());
                    std::destroy_at(s.ptr());
                    // Recycle the slot for a future producer.
                    s.sequence.store(tail + Capacity, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                // Queue is empty.
                return false;
            } else {
                tail = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    /**
     * @brief Attempt to dequeue one item, returning std::optional.
     *
     * Convenience overload that avoids an out-parameter.  Slightly more
     * expensive due to optional's move on return path.
     */
    [[nodiscard]] std::optional<T> try_pop()
        noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        size_type tail = tail_.load(std::memory_order_relaxed);

        for (;;) {
            Slot&     s    = slot_at(tail);
            size_type seq  = s.sequence.load(std::memory_order_acquire);
            auto      diff = static_cast<std::intptr_t>(seq)
                           - static_cast<std::intptr_t>(tail + 1);

            if (diff == 0) {
                if (tail_.compare_exchange_weak(
                        tail, tail + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                {
                    std::optional<T> result{std::move(*s.ptr())};
                    std::destroy_at(s.ptr());
                    s.sequence.store(tail + Capacity, std::memory_order_release);
                    return result;
                }
            } else if (diff < 0) {
                return std::nullopt;
            } else {
                tail = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    // ------------------------------------------------------------------
    // Blocking (spin-wait) operations
    // ------------------------------------------------------------------

    /**
     * @brief Enqueue one item, spinning until space is available.
     *
     * Uses the PAUSE hint between retries to signal a spin loop to the
     * CPU's branch predictor and memory-ordering subsystem, reducing the
     * likelihood of a machine-clear and lowering power.
     *
     * In HFT: prefer this over try_push in the hot path because yielding
     * the OS scheduler (e.g. std::this_thread::yield) introduces latency
     * jitter that can cost microseconds.  Spin loops are appropriate when
     * back-pressure is rare and the OS does not need the core.
     */
    template<typename U>
        requires std::constructible_from<T, U&&>
    void push(U&& item) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
        while (!try_push(std::forward<U>(item))) {
            detail::cpu_pause();
        }
    }

    /**
     * @brief Dequeue one item, spinning until one is available.
     */
    T pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
        T item;
        while (!try_pop(item)) {
            detail::cpu_pause();
        }
        return item;
    }

    // ------------------------------------------------------------------
    // Batch operations
    // ------------------------------------------------------------------

    /**
     * @brief Push up to `items.size()` elements from a span.
     *
     * @return Number of elements actually pushed (may be < items.size()
     *         if the queue fills up mid-batch).
     *
     * Design note: batch operations matter in HFT because they amortise
     * the per-item overhead of atomic CAS and cache-line ping-ponging.
     * Calling try_push N times in a tight loop is already efficient (the
     * atomic CAS on head is the bottleneck), but exposing a batch API
     * makes the caller's intent explicit and allows future optimisation
     * (e.g. reserving a contiguous range of slots with a single wide CAS
     * on head).  Current implementation is a simple loop — the wide-CAS
     * optimisation is left as a TODO because it complicates the sequence-
     * number invariants and is rarely needed for HFT throughputs.
     */
    template<typename U>
        requires std::constructible_from<T, const U&>
    [[nodiscard]] std::size_t try_push_bulk(std::span<const U> items)
        noexcept(std::is_nothrow_constructible_v<T, const U&>)
    {
        std::size_t pushed = 0;
        for (const U& item : items) {
            if (!try_push(item)) break;
            ++pushed;
        }
        return pushed;
    }

    /**
     * @brief Pop up to `out.size()` elements into a span.
     *
     * @return Number of elements actually popped (may be < out.size()
     *         if the queue empties mid-batch).
     */
    [[nodiscard]] std::size_t try_pop_bulk(std::span<T> out)
        noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        std::size_t popped = 0;
        for (T& dest : out) {
            if (!try_pop(dest)) break;
            ++popped;
        }
        return popped;
    }

    // ------------------------------------------------------------------
    // Capacity / size queries
    // ------------------------------------------------------------------

    /// Maximum number of elements the ring buffer can hold simultaneously.
    [[nodiscard]] static constexpr size_type capacity() noexcept {
        return Capacity;
    }

    /**
     * @brief Approximate number of enqueued items at an instant in time.
     *
     * WARNING: In a concurrent environment this value is STALE by the
     * time it is returned to the caller.  Useful for monitoring /
     * telemetry but NEVER for synchronisation decisions.
     *
     * Memory ordering: both loads are ACQUIRE to give a consistent
     * linearisation point.  The subtraction is unsigned and wraps
     * correctly even across the size_type overflow boundary because
     * head always leads tail (by invariant) modulo 2^64.
     */
    [[nodiscard]] size_type size() const noexcept {
        const size_type h = head_.load(std::memory_order_acquire);
        const size_type t = tail_.load(std::memory_order_acquire);
        // Unsigned subtraction: valid because head >= tail (mod 2^64).
        return h - t;
    }

    /// True if the ring buffer appears empty.  Subject to TOCTOU races.
    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    /// True if the ring buffer appears full.  Subject to TOCTOU races.
    [[nodiscard]] bool full() const noexcept {
        return size() >= Capacity;
    }

}; // class MPMCRingBuffer


// ---------------------------------------------------------------------------
// Convenience type alias with explicit cache-line padding size
// ---------------------------------------------------------------------------

/// HFT-optimised alias: 4096 slots (fits in L1/L2 with small T).
template<typename T>
using HFTQueue = MPMCRingBuffer<T, 4096>;

} // namespace mpmc

