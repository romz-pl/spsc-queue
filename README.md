# SPSC Lock-Free Queue

A **Single Producer – Single Consumer (SPSC) lock-free queue** implemented in C++17. Designed for high-performance, low-latency inter-thread communication without the overhead of mutexes or condition variables.

---

## ✨ Features

- **Lock-free** — no mutexes, no blocking, no priority inversion
- **Wait-free** for both producer and consumer under typical conditions
- **Cache-friendly** — head and tail indices are placed on separate cache lines to eliminate false sharing
- **Zero dynamic allocation** after construction — fixed-capacity ring buffer
- **Linearizable** — operations appear to take effect atomically at a single point in time
- **Header-only** — drop a single file into your project and go

---

## 📐 How It Works

The queue is built on a **circular ring buffer** of a fixed capacity. Two atomic indices — `head_` (read by the consumer) and `tail_` (written by the producer) — track the state of the buffer.

```
  tail_ ──► [  ] [  ] [ X ] [ X ] [ X ] [  ] [  ]
                        ▲                 ▲
                     consumer           producer
                      reads             writes
                     (head_)            (tail_)
```

- The **producer** loads `tail_`, checks that the next slot is not `head_`, writes the item, then stores the new `tail_` with `release` ordering.
- The **consumer** loads `head_`, checks that it is not `tail_`, reads the item, then stores the new `head_` with `release` ordering.
- Because only one thread ever writes `tail_` and only one thread ever writes `head_`, no compare-and-swap (CAS) loop is needed — a plain atomic store is sufficient.

### Memory ordering

| Operation | Ordering |
|-----------|----------|
| `enqueue` — check `head_` | `acquire` |
| `enqueue` — publish new `tail_` | `release` |
| `dequeue` — check `tail_` | `acquire` |
| `dequeue` — publish new `head_` | `release` |

This acquire/release pairing ensures that all writes performed by the producer before storing `tail_` are visible to the consumer after it loads `tail_`.

---

## 📁 Repository Structure

```
spsc-lock-free-queue/
│
├── include/
│   └── spsc_queue.hpp          # Header-only queue implementation
│
├── src/
│   └── main.cpp                # Usage example / demo
│
├── tests/
│   ├── test_basic.cpp          # Correctness tests (single-threaded)
│   ├── test_concurrent.cpp     # Stress tests (two threads)
│   └── test_edge_cases.cpp     # Full / empty / wrap-around edge cases
│
├── benchmarks/
│   ├── bench_spsc.cpp          # Throughput & latency benchmarks
│   └── results/                # Pre-recorded benchmark results (Markdown + CSV)
│
├── docs/
│   ├── design.md               # Detailed design notes & trade-offs
│   └── memory_model.md         # C++ memory model walk-through for this queue
│
├── .github/
│   └── workflows/
│       └── ci.yml              # GitHub Actions: build, test, sanitizers
│
├── CMakeLists.txt              # CMake build configuration
├── LICENSE                     # MIT License
└── README.md                   # This file
```

---

## 🚀 Quick Start

### Prerequisites

- C++17-capable compiler (GCC ≥ 7, Clang ≥ 5, MSVC ≥ 19.14)
- CMake ≥ 3.15

### Build

```bash
git clone https://github.com/romz-pl/spsc-queue.git
cd spsc-queue
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Run the demo

```bash
./build/src/spsc-demo
```

### Run the tests

```bash
cd build && ctest --output-on-failure
```

---

## 🔧 Usage

```cpp
#include "spsc_queue.hpp"

// Create a queue with a capacity of 1024 slots
SPSCQueue<int, QUEUE_CAP> q;

// ---- Producer ----
std::thread producer([&] {
    for (int i = 0; i < ITEM_COUNT; ++i)
        q.push_blocking(i);

    q.push_blocking(SENTINEL);   // tell the consumer we're done
});

// ---- Consumer ----
std::thread consumer([&] {
    long long sum      = 0;
    int       received = 0;

    while (true) {
        int val = q.pop_blocking();
        if (val == SENTINEL) break;
    }
});
```

### API

| Method | Description |
|--------|-------------|
| `SPSCQueue(size_t capacity)` | Constructs a queue with the given fixed capacity |
| `bool enqueue(const T& item)` | Tries to enqueue an item; returns `false` if the queue is full |
| `bool enqueue(T&& item)` | Move-enqueue overload |
| `bool dequeue(T& item)` | Tries to dequeue into `item`; returns `false` if the queue is empty |
| `bool empty() const` | Returns `true` if the queue contains no items (approximate) |
| `bool full() const` | Returns `true` if no more items can be enqueued (approximate) |
| `size_t size() const` | Returns the approximate number of items currently in the queue |
| `size_t capacity() const` | Returns the maximum number of items the queue can hold |

> **Note:** `empty()`, `full()`, and `size()` are inherently approximate in a concurrent setting and should be used for diagnostics only, not for synchronization decisions.

---

## 📊 Performance

Benchmarks were run on an Intel Core i7-12700K, Ubuntu 22.04, GCC 12 (`-O2`), with the producer and consumer pinned to separate physical cores.

| Metric | Value |
|--------|-------|
| Throughput | ~450 million ops/sec |
| Average round-trip latency | ~12 ns |
| Cache line size assumed | 64 bytes |

> Results will vary depending on hardware, compiler flags, and workload. Run `bench_spsc` on your own machine for representative numbers.

---

## ⚠️ Constraints & Limitations

- **Exactly one producer thread** and **exactly one consumer thread** — using this queue with multiple producers or consumers results in undefined behaviour.
- **Fixed capacity** — the ring buffer does not grow dynamically. Size your queue according to your worst-case burst.
- **Non-copyable, non-movable** — the queue object itself cannot be copied or moved after construction.

---

## 🧪 Testing & Sanitizers

The CI pipeline runs the test suite under:

- **ThreadSanitizer (TSan)** — detects data races
- **AddressSanitizer (ASan)** — detects memory errors
- **UndefinedBehaviorSanitizer (UBSan)** — detects undefined behaviour

To run locally with sanitizers:

```bash
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan
cd build-tsan && ctest
```

---

## 📖 Further Reading

- [C++ Memory Model (cppreference)](https://en.cppreference.com/w/cpp/atomic/memory_order)
- Herb Sutter — *"Lock-Free Programming"* (CppCon 2014)
- Dmitry Vyukov — [*"Single-Producer Single-Consumer Queue"*](https://www.1024cores.net/home/lock-free-algorithms/queues/unbounded-spsc-queue)
- Paul E. McKenney — *"Is Parallel Programming Hard, And, If So, What Can You Do About It?"*

---

## 📄 License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.

---

## 🤝 Contributing

Contributions, bug reports, and suggestions are welcome! Please open an issue or submit a pull request. Make sure your changes pass all existing tests and sanitizer runs before submitting.
