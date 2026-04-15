# Description of Concurrent Tests



| # | Test name | What it stresses |
|---|-----------|-----------------|
| 1 | `OrderingAndNoLoss` | Verifies strict FIFO ordering across 1M items — no reordering, no drops |
| 2 | `ThroughputTinyQueue` | Maximum contention with a 2-slot queue; validates checksum so no corruption |
| 3 | `NonBlockingPushPopCoherence` | Confirms `push`/`pop` return values are always coherent under racing conditions |
| 4 | `LargePayloadIntegrity` | 64-byte aligned structs to expose false-sharing or torn-write bugs |
| 5 | `TimeBoundedStress` | Runs flat-out for 500 ms; checks nothing is lost when the stop signal fires |
| 6 | `FullAndEmptyBoundaryConditions` | Forces the queue to hit both `full` and `empty` states repeatedly |
| 7 | `ModuloCorrectnessVariousCapacities` | Exercises the `% N` wrap-around at capacities 2, 3, 7, 8, 31, 128 |
| 8 | `BlockingAPINoDeadlock` | Watchdog thread kills the test if `push_blocking`/`pop_blocking` deadlock |
| 9 | `BurstPattern` | Producer sends 50-item bursts with micro-pauses; catches starvation bugs |
| 10 | `NoTornReadsOn64BitValues` | Encodes a magic XOR invariant into every value; any torn read is detected |

**To build and run:**
```bash
g++ -std=c++17 -O2 -pthread spsc_queue_stress_test.cpp \
    -lgtest -lgtest_main -o spsc_stress && ./spsc_stress
```
Or with CMake, link `GTest::GTest` and `pthread`.
The `Barrier` helper inside the file ensures threads actually start simultaneously without relying on `sleep`.
