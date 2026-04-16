# Quill Logger in High-Frequency Trading

## What is Quill?

Quill is an asynchronous, low-latency C++ logging library (C++17 and beyond) engineered around a single core design principle: **minimize the latency impact on the hot path**. It achieves this by offloading all I/O, formatting, and string operations to a dedicated backend thread, leaving the trading thread to do nothing more than copy a few bytes into a lock-free queue.

---

## Architecture Deep-Dive

### Threading Model

Quill operates on a **producer-consumer architecture** with strict thread role separation:

- **Frontend (trading threads):** Enqueue log records into a per-thread, single-producer single-consumer (SPSC) queue. No formatting, no I/O, no allocation on the hot path.
- **Backend (logger thread):** A single, dedicated thread drains all frontend queues, formats messages, and dispatches to sinks (file, console, etc.).

This means the trading thread's `LOG_INFO(...)` call is essentially a bounded `memcpy` + an atomic store — often under **10–50 nanoseconds** on modern hardware.

### Queue Design

Each frontend thread gets its own **bounded, cache-line-aligned SPSC ring buffer**. The key properties are:

- **Wait-free on the producer side:** No CAS loops, no contention. The producer simply checks if space is available and writes.
- **Avoids false sharing:** Each thread's queue metadata is on its own cache line.
- **Configurable capacity:** You tune queue size to absorb bursts without blocking the trading thread.

```cpp
// Configure per-thread queue size at startup (power of 2, in bytes)
quill::BackendOptions backend_options;
backend_options.transit_event_buffer_initial_capacity = 4096; // events
quill::Backend::start(backend_options);
```

---

## Integration into an HFT System

### 1. Installation and Build Integration (CMake)

```cmake
include(FetchContent)
FetchContent_Declare(
  quill
  GIT_REPOSITORY https://github.com/odygrd/quill.git
  GIT_TAG        v7.0.0
)
FetchContent_MakeAvailable(quill)

target_link_libraries(hft_engine PRIVATE quill::quill)
```

### 2. Backend Thread Initialization (done once at startup, off the hot path)

```cpp
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/FileSink.h"

void init_logging() {
    quill::BackendOptions opts;

    // Pin the backend thread to an isolated core (e.g., core 3)
    // to avoid OS scheduler interference on trading cores
    opts.backend_cpu_affinity = 3;

    // Busy-spin the backend thread for minimum wake-up latency
    opts.sleep_duration = std::chrono::nanoseconds{0};

    quill::Backend::start(opts);
}
```

Pinning the backend to a **dedicated, isolated CPU core** (via `isolcpus` in the kernel boot parameters) is critical — it prevents the backend's I/O operations from evicting the trading thread's working set from L1/L2 cache.

### 3. Logger and Sink Setup (startup, off hot path)

```cpp
quill::Logger* setup_logger(const std::string& log_path) {
    // Async file sink with large buffer to absorb burst writes
    auto file_sink = quill::Frontend::create_or_get_sink<quill::FileSink>(
        log_path,
        []() {
            quill::FileSinkConfig cfg;
            cfg.set_open_mode('w');
            cfg.set_do_fsync_at_flush(false); // never fsync on hot path
            return cfg;
        }(),
        quill::FileEventNotifier{}
    );

    return quill::Frontend::create_or_get_logger(
        "HFT",
        std::move(file_sink),
        quill::PatternFormatterOptions{
            "%(time) [%(log_level)] %(message)",
            "%H:%M:%S.%Qns",  // nanosecond-resolution timestamps
            quill::Timezone::LocalTime
        }
    );
}
```

### 4. Hot Path Logging

This is the critical section. The key guarantee is that **no heap allocation, no syscall, and no format string processing** occurs on the trading thread.

```cpp
// In your order execution / market data handler:
void on_market_data(Logger* logger, const MarketTick& tick) {
    // LOG_INFO uses __builtin_expect and a lock-free enqueue.
    // For trivially copyable types, arguments are memcpy'd directly
    // into the SPSC queue — zero heap allocation.
    LOG_INFO(logger, "Tick: sym={} bid={:.5f} ask={:.5f} qty={}",
             tick.symbol, tick.bid, tick.ask, tick.quantity);
}

void on_order_fill(Logger* logger, uint64_t order_id, double fill_price) {
    LOG_INFO(logger, "Fill: oid={} price={:.5f}", order_id, fill_price);
}
```

For **non-trivially-copyable types** (e.g., `std::string`), Quill copies the string data into the queue buffer at enqueue time — the backend thread does not touch the original string, which is safe even if the string is stack-allocated and goes out of scope.

### 5. Compile-Time Log Level Filtering

You can eliminate logging overhead entirely for filtered-out levels at **compile time** using preprocessor definitions, resulting in zero-cost abstractions:

```cpp
// In CMakeLists.txt or compiler flags:
// -DQUILL_COMPILE_ACTIVE_LOG_LEVEL=QUILL_COMPILE_ACTIVE_LOG_LEVEL_INFO
// This means DEBUG/TRACE calls compile to nothing — no branch, no queue write.
```

### 6. Custom Timestamping with TSC Clock

For HFT, `CLOCK_REALTIME` syscalls are too slow and too jittery. Quill supports **RDTSC-based timestamping**, which reads the CPU's timestamp counter directly — a single instruction with ~1ns latency:

```cpp
quill::BackendOptions opts;
opts.rdtsc_resync_interval = std::chrono::milliseconds{500};
// Quill will calibrate TSC → wall clock periodically on the backend
quill::Backend::start(opts);
```

The frontend records `__rdtsc()` inline; the backend converts it to a wall-clock timestamp during formatting — the TSC read on the hot path costs ~3–5 clock cycles.

### 7. Structured Logging for Custom Types (Market Data structs)

```cpp
// Register a custom formatter so the trading thread only copies the struct
template <>
struct fmtquill::formatter<OrderBook> {
    auto format(const OrderBook& ob, fmtquill::format_context& ctx) const {
        return fmtquill::format_to(ctx.out(),
            "OrderBook{{sym={}, bids={}, asks={}}}",
            ob.symbol, ob.bid_depth, ob.ask_depth);
    }
};

// Usage — struct is memcpy'd into queue; formatting happens on backend thread
LOG_DEBUG(logger, "Book update: {}", order_book);
```

### 8. Queue Overflow Strategy

In an HFT system, **blocking the trading thread is unacceptable**. Configure Quill to drop log records silently if the queue is full, rather than stalling:

```cpp
// Use unbounded queue or handle overflow policy
quill::FrontendOptions frontend_options;
frontend_options.queue_type = quill::QueueType::UnboundedBlocking;
// Or: UnboundedNoMaxLimit, BoundedDropping
```

`BoundedDropping` is often preferred in production: the queue has a fixed, cache-friendly size, and overflow records are silently discarded — preserving trading thread latency at the cost of completeness during log storms.

---

## Latency Profile (Typical Numbers on Modern x86-64)

| Operation | Approximate Latency |
|---|---|
| `LOG_INFO` on hot path (trivial args) | 10–40 ns |
| `LOG_INFO` on hot path (std::string copy) | 40–100 ns |
| RDTSC timestamp capture | ~3–5 cycles |
| Backend formatting + file write (async) | 1–10 µs (off hot path) |

---

## Pros and Cons

### ✅ Pros

**1. Exceptional Hot-Path Latency**
The SPSC lock-free queue and zero-allocation frontend design deliver some of the lowest logging latencies available in any C++ logging framework. The ~10–40 ns range is competitive even against hand-rolled logging solutions.

**2. Zero Formatting on the Trading Thread**
Format string processing, integer-to-string conversion, and timestamp formatting all happen on the backend thread. The trading thread is fully isolated from these CPU-intensive operations.

**3. RDTSC Clock Support**
Native support for TSC-based timestamps avoids `clock_gettime` syscalls on the critical path, providing both lower latency and higher timestamp precision — both essential for regulatory audit trails and latency analysis.

**4. Per-Thread SPSC Queues**
No shared mutex or atomic contention between trading threads. Each thread operates entirely independently on its own queue, making the system scale cleanly to multi-strategy, multi-instrument architectures.

**5. Compile-Time Level Elimination**
Filtering at compile time ensures that disabled log levels incur exactly zero runtime cost — not even a branch prediction miss.

**6. Bounded Memory Footprint**
Queue sizes are fixed and known at startup, making memory usage deterministic and cache behavior predictable — both critical for consistent latency in HFT.

**7. Modern C++ Design**
Built on C++17 with support for structured bindings, `std::string_view`, and custom `fmt`-style formatters. Integrates naturally into modern HFT codebases.

**8. Active Maintenance and HFT-Aware Development**
Quill is actively developed with HFT use cases explicitly in mind. The author benchmarks and documents latency characteristics, which is rare among logging libraries.

---

### ❌ Cons

**1. Queue Overflow = Silent Log Loss**
With `BoundedDropping`, records are silently discarded during bursts. In a post-trade audit or incident investigation, missing log records can be a serious regulatory or operational problem. Mitigating this requires careful queue sizing and monitoring of drop counters.

**2. Backend Thread Resource Contention**
Even with CPU pinning, the backend thread competes for L3 cache bandwidth, memory bus bandwidth, and NUMA resources. On systems with aggressive NUMA topologies or overloaded memory channels, backend I/O can introduce subtle latency jitter to trading threads sharing the same socket.

**3. Not a Zero-Copy Logger**
Arguments are copied into the queue buffer — they are not logged by reference. For large structs or deep data structures, this copy cost can dominate the hot-path latency. Careful struct design (small, cache-line-friendly) is required to keep copy costs low.

**4. Delayed Visibility**
Since all formatting and I/O is deferred to the backend, log entries are not immediately visible in files. In a crash scenario, the queue may not have been fully drained, leading to loss of the most recent pre-crash log records — potentially the most diagnostically valuable ones.

**5. Complexity of Core Isolation Setup**
Achieving the advertised latency figures requires careful OS-level configuration: `isolcpus`, `nohz_full`, disabling IRQ affinity, and NUMA-aware memory allocation. Without this infrastructure, Quill's latency advantage over simpler loggers narrows considerably.

**6. `fmt` Dependency and Compile Times**
Quill depends on the `{fmt}` library for formatting. In large HFT codebases with many translation units, the template-heavy formatting machinery can meaningfully increase build times.

**7. No Built-In Network/Structured Log Sinks**
Unlike some enterprise logging frameworks, Quill does not ship with built-in sinks for network streaming (e.g., to a centralized log aggregator) or structured formats like JSON or FIX protocol. These must be implemented as custom sinks.

**8. Single Backend Thread**
All frontend queues are drained by one backend thread. If the aggregate log volume across all trading strategies exceeds what a single thread can format and write, backpressure builds and queues fill. This is rarely a problem in practice but must be capacity-planned for.

---

## Summary Verdict

Quill is one of the most technically sound choices for logging in a C++ HFT system. Its architectural decisions — SPSC queues, deferred formatting, RDTSC clocks, compile-time filtering — are precisely aligned with HFT latency requirements. The main engineering discipline required is careful infrastructure setup (CPU isolation, queue sizing, crash-safety planning) and acceptance that log completeness is traded against latency. For teams with the operational maturity to manage those tradeoffs, Quill is an excellent production-grade solution.

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Give a detailed explanation of how to use the Quill Logger in a high-frequency trading system that is implemented in C++. This description is intended for a computer science expert. Next, evaluate the pros and cons of using the Quill Logger in a high-frequency trading system.
> 
