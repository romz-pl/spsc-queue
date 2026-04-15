# SPDLog in High-Frequency Trading Systems

## Overview

SPDLog is a header-only, asynchronous-capable C++ logging library built on top of the `{fmt}` formatting library. In HFT contexts, logging is a paradox: you need comprehensive audit trails and diagnostic data, but any I/O operation that blocks the hot path is catastrophic. SPDLog's architecture navigates this tension better than most alternatives.

---

## Core Architecture Concepts Relevant to HFT

### 1. Synchronous vs. Asynchronous Loggers

SPDLog offers two fundamental logger models:

**Synchronous loggers** block the calling thread until the log record is fully flushed. This is **never acceptable on the hot path** in HFT. Every microsecond of blocking is a missed opportunity or worse — a stale quote.

**Asynchronous loggers** decouple the logging call from the I/O operation using a **lock-free, bounded MPSC (multi-producer, single-consumer) ring buffer**. The calling thread enqueues a log record and returns immediately. A dedicated background thread drains the queue and performs the actual I/O.

```cpp
#include "spdlog/spdlog.h"
#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"

// Initialize the thread pool: queue size must be a power of 2
// 1 background thread is typically sufficient; more can cause contention
spdlog::init_thread_pool(1 << 16, 1); // 65536 slot queue, 1 I/O thread

auto async_logger = spdlog::create_async<spdlog::sinks::basic_file_sink_mt>(
    "hft_logger", "/var/log/hft/trading.log"
);
```

The queue size is critical. A queue that is too small will cause **overflow drops** (or blocking, depending on overflow policy); too large wastes memory and pollutes CPU caches.

---

### 2. Overflow Policies

SPDLog exposes two overflow strategies via `async_overflow_policy`:

```cpp
// Drop the message silently — preferred on hot path to avoid blocking
auto async_logger = spdlog::create_async_nb<spdlog::sinks::basic_file_sink_mt>(
    "hft_logger", "/var/log/hft/trading.log"
);
// _nb = non-blocking, uses overrun_oldest policy by default
```

| Policy | Behavior | HFT Suitability |
|---|---|---|
| `block` | Calling thread blocks until space available | ❌ Never on hot path |
| `overrun_oldest` | Oldest record is silently dropped | ✅ Preferred — bounded latency |

In HFT you almost always prefer `overrun_oldest`. A dropped log is recoverable (reconstruct from market data feeds); a blocked order thread is not.

---

### 3. Sink Architecture and Composability

SPDLog uses a **sink abstraction** — the logger itself is transport-agnostic. Sinks handle actual output. You can attach multiple sinks to a single logger, and each sink can have its own log level filter.

```cpp
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

// Rotating file sink: max 512 MB per file, 5 rotations
auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
    "/var/log/hft/trading.log", 1024 * 1024 * 512, 5
);
file_sink->set_level(spdlog::level::debug);

// Console sink for ops visibility — only warnings and above
auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
console_sink->set_level(spdlog::level::warn);

// Compose into a single async logger
spdlog::init_thread_pool(1 << 16, 1);
auto logger = std::make_shared<spdlog::async_logger>(
    "hft",
    spdlog::sinks_init_list{file_sink, console_sink},
    spdlog::thread_pool(),
    spdlog::async_overflow_policy::overrun_oldest
);
spdlog::register_logger(logger);
```

For HFT, a **memory-mapped file sink** or a **custom binary sink** (not built-in but easily implemented) is often preferred over a text file sink for throughput and post-hoc analysis.

---

### 4. Log Level Gating and Compile-Time Stripping

One of the most important HFT optimizations: **zero-cost logging at disabled levels**. SPDLog supports compile-time level stripping so that disabled log calls compile to nothing — no argument evaluation, no function call overhead whatsoever.

```cpp
// In CMakeLists.txt or build flags:
// -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_WARN

// This call compiles to a no-op in production builds:
SPDLOG_DEBUG("Order book depth: {}", order_book.depth()); // Zero cost

// This call remains active:
SPDLOG_WARN("Latency spike detected: {} µs", latency_us);
```

Use the `SPDLOG_*` macros (not the `logger->debug(...)` API) because only the macros trigger compile-time elimination. The runtime API always evaluates its arguments.

---

### 5. Custom Formatters and Pattern Optimization

The default text formatter is expensive. For HFT, minimize formatting work on the hot path:

```cpp
// Minimal pattern: no color codes, no thread ID lookup, nanosecond timestamps
logger->set_pattern("[%Y-%m-%dT%H:%M:%S.%f] [%l] %v");

// Even better: use a custom formatter that avoids strftime entirely
// and uses TSC-based timestamps (see below)
```

#### TSC-Based Timestamping

`std::chrono::system_clock` involves a syscall on many Linux kernels. In HFT, you want **RDTSC-based timestamps** pinned to a known clock frequency:

```cpp
#include "spdlog/pattern_formatter.h"

class TscTimestampFlag : public spdlog::custom_flag_formatter {
    uint64_t tsc_hz_; // Calibrated at startup
public:
    explicit TscTimestampFlag(uint64_t tsc_hz) : tsc_hz_(tsc_hz) {}

    void format(const spdlog::details::log_msg&, const std::tm&,
                spdlog::memory_buf_t& dest) override {
        uint64_t tsc;
        __asm__ volatile("rdtsc; shl $32, %%rdx; or %%rdx, %0"
                         : "=a"(tsc) : : "rdx");
        auto ns = (tsc * 1'000'000'000ULL) / tsc_hz_;
        fmt::format_to(std::back_inserter(dest), "{}", ns);
    }

    std::unique_ptr<custom_flag_formatter> clone() const override {
        return spdlog::details::make_unique<TscTimestampFlag>(tsc_hz_);
    }
};

auto formatter = std::make_unique<spdlog::pattern_formatter>();
formatter->add_flag<TscTimestampFlag>('*', calibrated_tsc_hz).set_pattern("[%*] %v");
logger->set_formatter(std::move(formatter));
```

---

### 6. CPU Affinity and NUMA Awareness

Bind the SPDLog background I/O thread to a **non-trading core**, ideally on the same NUMA node as your file storage or network interface, to avoid cross-NUMA memory traffic:

```cpp
// After spdlog::init_thread_pool(...), get the underlying std::thread
// and set affinity. SPDLog doesn't expose this directly, so use a wrapper:

spdlog::init_thread_pool(1 << 16, 1);

// Reach into the thread pool to pin the I/O thread
auto tp = spdlog::thread_pool();
// Thread pool uses an internal thread — set affinity via /proc or pthread
// This is typically done by wrapping spdlog's thread pool initialization
// with a custom ThreadPool subclass or post-initialization affinity setting.

cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(LOGGING_CORE_ID, &cpuset); // e.g., core 15, isolated from trading cores
pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
```

---

### 7. Memory Allocation and Object Pooling

Each `spdlog::details::log_msg` that enters the async queue carries a `fmt::memory_buffer` for the formatted string. This triggers heap allocations on every log call unless you apply one of these strategies:

**A) Short String Optimization (SSO):** Keep log messages under ~256 bytes — `fmt::memory_buffer` uses inline storage up to this threshold.

**B) Custom Allocator Sink:** Implement a sink backed by a pre-allocated arena to eliminate per-message heap allocations entirely.

**C) Structured Binary Logging:** Instead of formatting strings on the hot path, enqueue raw structs and format them in the background thread:

```cpp
struct OrderEvent {
    uint64_t tsc;
    uint64_t order_id;
    double   price;
    int32_t  qty;
    uint8_t  side; // 0=bid, 1=ask
};

// Custom sink that accepts binary blobs
void log_order(const OrderEvent& evt) {
    // Serialize directly into a pre-allocated ring buffer — no spdlog involved
    // Reconstruct human-readable logs offline during post-trade analysis
}
```

This pattern is often called **deferred formatting** or **zero-copy logging**.

---

### 8. Flush Strategy

Never call `flush()` on the hot path. Configure flush triggers explicitly:

```cpp
// Flush every 500ms — adequate for audit logs, not for crash recovery
logger->flush_on(spdlog::level::err); // Flush immediately on errors only
spdlog::flush_every(std::chrono::milliseconds(500));
```

For crash resilience, pair this with `O_DSYNC` on the underlying file descriptor or use a write-ahead log (WAL) pattern external to SPDLog.

---

## Pros and Cons for HFT

### ✅ Pros

| Factor | Detail |
|---|---|
| **Header-only** | No shared library dependency; zero link-time friction; easy vendoring into a monorepo |
| **Async ring buffer** | Lock-free enqueue path; calling thread overhead is typically **20–80 ns** — acceptable for off-hot-path logging |
| **Compile-time stripping** | `SPDLOG_ACTIVE_LEVEL` removes disabled calls entirely; zero binary bloat in production builds |
| **{fmt} backend** | Compile-time format string validation; dramatically faster than `printf`/`iostream` |
| **Sink composability** | Route critical errors to a separate high-durability sink without restructuring logger calls |
| **Low dependency surface** | Only depends on `{fmt}`; no Boost, no protobuf, no runtime reflection |
| **Active maintenance** | Well-maintained, widely adopted; CVEs are patched promptly |
| **Customizability** | Custom sinks, custom formatters, and custom flag formatters are well-documented and straightforward to implement |

---

### ❌ Cons

| Factor | Detail |
|---|---|
| **Text formatting on the hot path** | Even with async logging, `fmt::format_to` is called on the **calling thread** before enqueue. For extremely tight loops, this is non-trivial |
| **Heap allocations** | `fmt::memory_buffer` heap-allocates for messages above the SSO threshold; incompatible with pre-allocated memory pools without custom work |
| **No built-in binary logging** | HFT systems often prefer binary/columnar log formats (e.g., Cap'n Proto, custom TLV) for post-trade analytics. SPDLog is fundamentally text-oriented |
| **No direct RDTSC support** | TSC-based timestamping requires a custom flag formatter (as shown above); not out of the box |
| **Background thread is opaque** | SPDLog doesn't expose its internal I/O thread handle cleanly, making CPU affinity pinning awkward |
| **Queue backpressure is binary** | Either block or drop — no backpressure signaling, no alerting when the queue is under pressure, requiring external monitoring |
| **No kernel bypass** | SPDLog writes via standard POSIX I/O; it cannot natively target `io_uring`, DPDK-backed storage, or RDMA. Achieving kernel-bypass logging requires a custom sink |
| **Single global thread pool** | `spdlog::thread_pool()` is process-global. Multiple independent subsystems sharing one pool can create unexpected coupling and queue contention |
| **Not designed for sub-100ns budgets** | For strategies where every nanosecond matters (e.g., co-located market-making), even SPDLog's async enqueue is too expensive on the critical path. Dedicated solutions like **NanoLog**, **Quill**, or custom lock-free binary journals are better suited |

---

## Summary Recommendation

SPDLog is an excellent choice for **off-hot-path logging** in HFT systems — risk checks, position updates, order state transitions, heartbeat monitoring, and post-trade audit trails. It should be used with `async_nb` mode, `overrun_oldest` policy, compile-time level stripping, minimal formatting patterns, and the I/O thread pinned to an isolated core.

For the **absolute hot path** (e.g., inside the order generation loop at sub-microsecond latency targets), SPDLog should be replaced with a **lock-free binary ring buffer** written to by the trading thread and drained by a dedicated logger thread — SPDLog can then serve as the downstream sink from that buffer, operating entirely outside the critical path. Libraries like **Quill** or **NanoLog** follow exactly this architecture and are worth evaluating as drop-in replacements if SPDLog's latency floor becomes a bottleneck.


---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Give a detailed explanation of how to use the SPDLog Logger in a high-frequency trading system that is implemented in C++. This description is intended for a computer science expert. Next, evaluate the pros and cons of using the SPDLog Logger in a high-frequency trading system.
