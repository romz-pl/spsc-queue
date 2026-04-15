# Measuring Execution Time in High-Frequency Trading Systems (C++)

## 1. The Core Challenge

In HFT, latency is measured in **nanoseconds**. Standard profiling tools (gprof, Valgrind) introduce prohibitive overhead. The measurement mechanism itself must be:

- **Non-intrusive** — sub-nanosecond overhead on the hot path
- **Monotonic** — immune to NTP adjustments and wall-clock drift
- **Serializing** — preventing CPU reordering across measurement boundaries
- **Core-local** — avoiding cross-socket coherence traffic

---

## 2. Hardware Primitives

### 2.1 RDTSC / RDTSCP

The **Time Stamp Counter** is the foundational primitive. It reads a 64-bit counter incremented every CPU cycle.

```cpp
#include <cstdint>

// Non-serializing — the CPU may reorder instructions across this
inline uint64_t rdtsc() {
    uint64_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (hi << 32) | lo;
}

// RDTSCP: serializes all prior loads (not stores), also returns core ID
inline uint64_t rdtscp(uint32_t& aux) {
    uint64_t lo, hi;
    __asm__ volatile ("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return (hi << 32) | lo;
}
```

**Critical caveat:** `RDTSC` alone is not a memory barrier. The CPU (and compiler) can reorder the reads relative to the code being timed.

### 2.2 Serialization with LFENCE

To prevent speculative execution from "leaking" across the measurement boundary:

```cpp
inline uint64_t rdtsc_serialized_start() {
    uint64_t lo, hi;
    // LFENCE: ensures all prior instructions retire before RDTSC executes
    __asm__ volatile (
        "lfence\n\t"
        "rdtsc\n\t"
        : "=a"(lo), "=d"(hi)
        :: "memory"
    );
    return (hi << 32) | lo;
}

inline uint64_t rdtsc_serialized_end() {
    uint64_t lo, hi;
    // RDTSCP serializes on the read side; LFENCE after prevents
    // subsequent instructions from executing before RDTSCP retires
    __asm__ volatile (
        "rdtscp\n\t"
        "lfence\n\t"
        : "=a"(lo), "=d"(hi)
        :: "rcx", "memory"
    );
    return (hi << 32) | lo;
}
```

This `LFENCE + RDTSC` / `RDTSCP + LFENCE` sandwich is the **Intel-recommended** pattern for measuring short sequences.

### 2.3 TSC Stability Requirements

Before trusting TSC, verify at startup:

```cpp
#include <fstream>
#include <string>

bool verify_tsc_invariant() {
    // Check /proc/cpuinfo for constant_tsc and nonstop_tsc
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line, flags_line;
    while (std::getline(cpuinfo, line)) {
        if (line.find("flags") != std::string::npos) {
            flags_line = line;
            break;
        }
    }
    bool constant  = flags_line.find("constant_tsc")  != std::string::npos;
    bool nonstop   = flags_line.find("nonstop_tsc")   != std::string::npos;
    return constant && nonstop;
}
```

`constant_tsc` — TSC increments at a fixed rate regardless of CPU frequency (P-states).
`nonstop_tsc` — TSC does not halt in C-states.

### 2.4 TSC-to-Nanosecond Calibration

```cpp
#include <time.h>

double calibrate_tsc_ghz() {
    // Measure TSC ticks over a known wall-clock interval
    struct timespec t1, t2;
    uint64_t tsc1, tsc2;

    clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
    tsc1 = rdtsc_serialized_start();

    // Spin for ~100ms — long enough to amortize syscall overhead
    struct timespec target = {0, 100'000'000L};
    nanosleep(&target, nullptr);

    tsc2 = rdtsc_serialized_end();
    clock_gettime(CLOCK_MONOTONIC_RAW, &t2);

    uint64_t ns_elapsed = (t2.tv_sec - t1.tv_sec) * 1'000'000'000ULL
                        + (t2.tv_nsec - t1.tv_nsec);
    uint64_t tsc_elapsed = tsc2 - tsc1;

    return static_cast<double>(tsc_elapsed) / static_cast<double>(ns_elapsed); // ticks/ns ≈ GHz
}

// Cache this at startup
static const double TSC_GHZ = calibrate_tsc_ghz();

inline double ticks_to_ns(uint64_t ticks) {
    return static_cast<double>(ticks) / TSC_GHZ;
}
```

---

## 3. RAII Scoped Timer

Wrap the measurement in a zero-overhead RAII guard to avoid manual bookkeeping:

```cpp
#include <cstdint>
#include <functional>

class ScopedTimer {
public:
    using Callback = std::function<void(uint64_t /*ticks*/, uint64_t /*start_tsc*/)>;

    explicit ScopedTimer(Callback cb)
        : cb_(std::move(cb))
        , start_(rdtsc_serialized_start())
    {}

    ~ScopedTimer() {
        uint64_t end = rdtsc_serialized_end();
        cb_(end - start_, start_);
    }

    // Non-copyable, non-movable — lifetime must match scope
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    Callback  cb_;
    uint64_t  start_;
};
```

Usage:

```cpp
void process_order(Order& order) {
    ScopedTimer t([](uint64_t ticks, uint64_t start) {
        logger.record(start, ticks);
    });
    // ... hot path code ...
}
```

In practice, for the **absolute hottest** paths, even the `std::function` indirection is too expensive. Replace it with a direct template parameter:

```cpp
template<typename F>
class ScopedTimerT {
public:
    explicit ScopedTimerT(F&& f)
        : cb_(std::forward<F>(f))
        , start_(rdtsc_serialized_start())
    {}

    ~ScopedTimerT() {
        cb_(rdtsc_serialized_end() - start_, start_);
    }

private:
    F        cb_;
    uint64_t start_;
};

// Deduction guide
template<typename F>
ScopedTimerT(F&&) -> ScopedTimerT<std::decay_t<F>>;
```

The lambda is now **inlined at compile time** — zero virtual dispatch, zero heap allocation.

---

## 4. Efficiently Passing Measurements to the Logger

This is where most naive implementations fail. Any blocking I/O, mutex, or syscall on the hot path destroys the latency profile you just measured.

### 4.1 The Fundamental Architecture: Lock-Free SPSC Ring Buffer

The canonical pattern is **asynchronous logging** via a lock-free Single-Producer Single-Consumer (SPSC) queue. The trading thread only writes a small, fixed-size record into a ring buffer. A dedicated logger thread drains it.

```cpp
#include <atomic>
#include <array>
#include <cstdint>

struct TimingRecord {
    uint64_t start_tsc;   // absolute TSC at start (for wall-clock reconstruction)
    uint64_t duration_tsc;// elapsed ticks
    uint32_t event_id;    // identifies which code path was measured
    uint32_t core_id;     // from RDTSCP aux register
};

template<std::size_t N>  // N must be a power of 2
class SPSCRingBuffer {
    static_assert((N & (N-1)) == 0, "N must be power of 2");
public:
    bool try_push(const TimingRecord& r) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = (head + 1) & (N - 1);
        if (next == tail_.load(std::memory_order_acquire))
            return false; // full
        buffer_[head] = r;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(TimingRecord& r) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false; // empty
        r = buffer_[tail];
        tail_.store((tail + 1) & (N - 1), std::memory_order_release);
        return true;
    }

private:
    alignas(64) std::atomic<std::size_t> head_{0};  // written by producer
    alignas(64) std::atomic<std::size_t> tail_{0};  // written by consumer
    std::array<TimingRecord, N> buffer_{};
};
```

Key design decisions:
- **`alignas(64)`** on the head/tail atomics — prevents **false sharing** between producer and consumer cache lines.
- `memory_order_release` on write, `memory_order_acquire` on read — sufficient ordering without a full `seq_cst` fence.
- No locks, no heap allocation, no syscalls on the hot path.

### 4.2 The Logger Thread

```cpp
#include <thread>
#include <fstream>

class AsyncTimingLogger {
public:
    AsyncTimingLogger(const std::string& path)
        : out_(path, std::ios::binary | std::ios::app)
        , running_(true)
        , thread_(&AsyncTimingLogger::drain_loop, this)
    {}

    ~AsyncTimingLogger() {
        running_.store(false, std::memory_order_release);
        thread_.join();
    }

    // Called from the hot path — must be wait-free
    bool record(uint64_t start_tsc, uint64_t duration_tsc,
                uint32_t event_id, uint32_t core_id) noexcept {
        return queue_.try_push({start_tsc, duration_tsc, event_id, core_id});
    }

private:
    void drain_loop() {
        // Pin logger thread to a dedicated isolated core
        pin_to_core(LOGGER_CORE_ID);

        TimingRecord rec;
        // Batch of records to write together
        constexpr int BATCH = 256;
        TimingRecord batch[BATCH];
        int batch_count = 0;

        while (running_.load(std::memory_order_acquire) || !queue_empty()) {
            while (batch_count < BATCH && queue_.try_pop(rec)) {
                batch[batch_count++] = rec;
            }
            if (batch_count > 0) {
                // Single write syscall for the whole batch
                out_.write(reinterpret_cast<const char*>(batch),
                           batch_count * sizeof(TimingRecord));
                batch_count = 0;
            } else {
                // Avoid busy-spinning at 100% when idle
                std::this_thread::yield();
            }
        }
        out_.flush();
    }

    SPSCRingBuffer<1 << 16>  queue_;   // 65536 slots
    std::ofstream            out_;
    std::atomic<bool>        running_;
    std::thread              thread_;
};
```

### 4.3 CPU Pinning and Isolation

Software alone is insufficient. The OS must be configured to keep the hot trading thread uninterrupted:

```cpp
#include <pthread.h>
#include <sched.h>

void pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

void set_fifo_priority(int priority = 99) {
    struct sched_param param{ .sched_priority = priority };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
}
```

And in `/etc/kernel/cmdline` (or `isolcpus=`):
```
isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
```

This removes the hot cores from the kernel's scheduler, RCU callbacks, and timer ticks — eliminating jitter sources at the OS level.

### 4.4 Avoiding printf / iostream on the Hot Path

Text formatting is far too expensive for the hot path. Log **raw binary** `TimingRecord` structs as shown above, and decode offline:

```python
# Offline decoder (Python)
import struct, sys

RECORD_FMT = "QQIi"  # start_tsc, duration_tsc, event_id, core_id
RECORD_SIZE = struct.calcsize(RECORD_FMT)

with open(sys.argv[1], "rb") as f:
    while chunk := f.read(RECORD_SIZE):
        start, dur, eid, core = struct.unpack(RECORD_FMT, chunk)
        print(f"event={eid} core={core} start={start} duration_ns={dur / 3.2:.1f}")
```

---

## 5. Measurement Pitfalls and How to Avoid Them

| Pitfall | Cause | Mitigation |
|---|---|---|
| **TSC migration** | Thread migrates to another core mid-measurement | Pin threads; use `RDTSCP` to detect core switches |
| **Warm-up bias** | Cold I-cache / D-cache inflating first samples | Discard first N samples; pre-fault all memory |
| **Compiler reordering** | Compiler hoists/sinks code past measurement point | `"memory"` clobber in the asm fence |
| **CPU reordering** | Speculative execution skews timestamps | `LFENCE` before start, `RDTSCP + LFENCE` at end |
| **Turbo Boost jitter** | CPU frequency varies between TSC calibration and measurement | Disable Turbo Boost in BIOS or via `cpupower` |
| **SMT interference** | Sibling hyperthread polluting cache | Disable SMT (`nosmt`); or dedicate full physical cores |
| **Ring buffer overflow** | Logger thread too slow to drain | Size buffer generously; alert on overflow; consider backpressure |

---

## 6. Summary Architecture

```
  ┌─────────────────────────────────────┐
  │         Trading Hot Path            │  Core 2 (isolated, SCHED_FIFO)
  │                                     │
  │  LFENCE → RDTSC → [work] →         │
  │  RDTSCP → LFENCE → try_push()      │
  └─────────────┬───────────────────────┘
                │ TimingRecord (24 bytes)
                │ lock-free SPSC ring
                ▼
  ┌─────────────────────────────────────┐
  │         Logger Thread               │  Core 3 (isolated, lower priority)
  │                                     │
  │  try_pop() → batch → write()        │
  │  (binary, unbuffered, O_DIRECT)     │
  └─────────────────────────────────────┘
                │
                ▼
        timing_log.bin  →  offline decoder → analysis / dashboards
```

The key insight is **total separation of concerns**: the hot path does nothing but take two TSC readings and write 24 bytes into a cache-local ring buffer. Every expensive operation — formatting, I/O, time conversion — is deferred to a separate core and processed asynchronously.

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Give a detailed explanation of how measure execution time in a high-frequency trading system implemented in C++. This description is intended for a computer science expert. Explain how the measured time can be eficiently passed to the logger.
