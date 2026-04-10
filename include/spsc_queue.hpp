#include <atomic>
#include <thread>
// #include <optional>
#include <cassert>

// ── Ring buffer ────────────────────────────────────────────────────────────────

template<typename T, size_t N>
struct SPSCQueue {
    static_assert(N >= 2, "capacity must be at least 2");

    T      buffer[N];
    std::atomic<size_t> head{0};   // producer writes, consumer reads
    std::atomic<size_t> tail{0};   // consumer writes, producer reads

    // Called only by the producer thread.
    bool push(const T& val) {
        const size_t h    = head.load(std::memory_order_relaxed);
        const size_t next = (h + 1) % N;

        if (next == tail.load(std::memory_order_acquire))
            return false;          // queue full

        buffer[h] = val;
        head.store(next, std::memory_order_release);
        return true;
    }

    // Called only by the consumer thread.
    bool pop(T& val) {
        const size_t t = tail.load(std::memory_order_relaxed);

        if (t == head.load(std::memory_order_acquire))
            return false;          // queue empty

        val = buffer[t];
        tail.store((t + 1) % N, std::memory_order_release);
        return true;
    }

    // Convenience wrapper — spins until space is available.
    void push_blocking(const T& val) {
        while (!push(val))
            std::this_thread::yield();
    }

    // Convenience wrapper — spins until an item is available.
    T pop_blocking() {
        T val;
        while (!pop(val))
            std::this_thread::yield();
        return val;
    }
};


