# Introduction to SPSC Lock-Free Queue

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
│   └── test_concurrent.cpp     # Stress tests (two threads)
│
├── docs/
|   ├── *.md
|   ├── *.svg
|   ├── test_basic.md            # Description of basic tests
|   └── test_concurrent.md       # Description of concurrent tests
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
./build/src/mpmc-demo
```

### Run the tests

```bash
cd build/tests && ctest --output-on-failure
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

## 📄 License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.

---

## 🤝 Contributing

Contributions, bug reports, and suggestions are welcome! Please open an issue or submit a pull request. Make sure your changes pass all existing tests and sanitizer runs before submitting.
