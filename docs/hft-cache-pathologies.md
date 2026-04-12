# Cache Pathologies in High-Frequency Trading Systems

## 1. Cache Misses

### Taxonomy and Mechanics

Modern HFT systems live and die by memory latency. The cache miss hierarchy maps directly to the latency budget:

| Level | Latency (cycles) | Latency (ns) @ 4 GHz |
|---|---|---|
| L1 hit | 4–5 | ~1.25 |
| L2 hit | 12–14 | ~3.5 |
| L3 hit | 40–50 | ~12 |
| DRAM (local) | 200–300 | ~65 |
| DRAM (remote NUMA) | 400–600 | ~130 |

Three orthogonal miss types compound in HFT:

**Compulsory (cold) misses** are unavoidable on first access. In HFT, this matters at strategy initialization and when a new instrument or counterparty record is touched for the first time — e.g., an exotic options leg that hasn't been active in the current session. Prefetcher-hostile access patterns (pointer chasing in linked order books, hash table traversal) amplify cold misses into sustained latency spikes.

**Capacity misses** occur when the working set exceeds cache capacity. An order book for a single liquid equity can consume 50–200 KB when including bid/ask ladders, participant metadata, and risk limits. A strategy trading 50 correlated instruments simultaneously easily exceeds the 512 KB–1 MB L2, forcing continuous eviction. The working set size `W` relative to cache size `C` determines the steady-state miss rate non-linearly — near `W ≈ C`, miss rate explodes due to thrashing.

**Conflict misses** arise from set-associativity limits. With a typical 8-way set-associative L1 (32 KB, 64-byte lines → 64 sets), data structures allocated at addresses that are multiples of the cache size map to identical sets. Two critical ring buffer entries or two frequently accessed order book levels can alias into the same set and evict each other on every access — a pathological pattern that manifests even when `W << C`. This is particularly insidious in HFT because many data structures are power-of-2 aligned by default.

### HFT-Specific Miss Patterns

**Pointer chasing in order books.** A traditional price-level linked list forces serial dependent loads: each next-pointer dereference cannot begin until the previous load completes (load-to-use latency). For a 10-level deep book walk, this serializes 10 × ~65 ns DRAM accesses, producing ~650 ns of irreducible latency — catastrophic against a sub-microsecond alpha signal decay curve.

**Hash table probing under load.** Open-addressing hash tables with high load factors (> 0.7) generate probe chains. Each probe is a potential cache miss. In a symbol lookup or order-ID resolution path, even two cache misses at 65 ns each consume budget that a competing firm may spend executing an entire trade.

**Prefetcher defeat.** Hardware prefetchers (stream detectors, stride detectors) work well for sequential or strided access. Non-linear patterns — binary search in price arrays, scattered risk limit checks — prevent prefetcher engagement entirely, ensuring every access is a demand miss to DRAM.

---

## 2. False Sharing

### The Mechanism

A cache coherence protocol (MESI, MESIF, or MOESI depending on the CPU microarchitecture) maintains consistency at **cache line granularity** (64 bytes on x86). False sharing occurs when two logically independent variables co-reside on the same cache line and are written by different threads or cores.

When Core A writes variable `x` and Core B writes variable `y`, and both sit within the same 64-byte line:

```
[ ... x (8 bytes) ... y (8 bytes) ... padding (48 bytes) ]
  ↑ Core A owns        ↑ Core B owns
        ← single coherence unit →
```

The sequence under MESI:
1. Core A has line in **Modified** state, writes `x`.
2. Core B issues a write to `y`. Sends **BusUpgr** (invalidate request) to all caches.
3. Core A receives invalidation, must **write-back** dirty line to L3/DRAM, transitions to **Invalid**.
4. Core B fetches the now-clean line, acquires **Exclusive**, modifies `y`, transitions to **Modified**.
5. Core A issues a subsequent write to `x` — repeat.

Each round trip traverses the inter-core interconnect (ring bus or mesh), costing 40–100 ns per ownership transfer. In a tight loop, false sharing can serialize what should be fully parallel operations — the effective throughput collapses to a fraction of theoretical maximums.

### HFT False Sharing Vectors

**Shared ring buffer metadata.** A producer-consumer SPSC ring buffer has a `head` (written by producer) and `tail` (written by consumer). If naively placed in the same struct:

```cpp
struct RingBuffer {
    uint64_t head;  // producer writes
    uint64_t tail;  // consumer writes  ← same cache line!
    T* data;
};
```

Every enqueue bounces the line from producer core to consumer core and back. The fix is explicit padding or `alignas`:

```cpp
struct alignas(64) RingBuffer {
    alignas(64) std::atomic<uint64_t> head;
    alignas(64) std::atomic<uint64_t> tail;
};
```

**Per-thread statistics counters.** Throughput, fill-rate, and PnL accumulators written per-thread but laid out in a shared array:

```cpp
struct Stats { uint64_t fills; uint64_t rejects; };
Stats thread_stats[NUM_THREADS]; // contiguous → false sharing
```

With 8-byte counters and 64-byte cache lines, up to 8 threads share a line. Solution: pad each `Stats` to 64 bytes or use thread-local storage.

**Market data feed dispatcher.** A multi-threaded feed handler that writes per-symbol sequence numbers into a contiguous array suffers false sharing across symbols in the same price range — exactly the symbols a correlation-trading strategy cares about simultaneously, compounding the effect.

**Spin-lock state variables.** A naive `std::atomic<bool> locked` at 1 byte. Packing multiple locks into a struct puts them on shared lines. When different threads contend on different locks, the coherence traffic generates phantom contention even between logically unrelated critical sections.

### Measuring False Sharing

False sharing is invisible to functional correctness and notoriously hard to diagnose. The key hardware performance counter is `MEM_LOAD_RETIRED.L1_MISS` combined with `OFFCORE_RESPONSE` events. Intel VTune's "Memory Access" analysis and `perf c2c` (cache-to-cache) are the canonical tools. The signature is high L3 hit rate (not DRAM — the line is valid in another core's cache) with anomalously high inter-core latency, visible as `HITM` (Hit Modified) events in `perf c2c` output.

---

## 3. Cache Pollution

### Definition and Distinction

Cache pollution is the displacement of **hot** (frequently accessed, latency-critical) data by **cold** data that transiently passes through the cache but is not needed again soon. Unlike false sharing (a coherence problem) or capacity misses (a working-set-size problem), pollution is a **temporal locality** problem: data is evicted before it has been reused.

The eviction policy (pseudo-LRU in most modern caches) cannot distinguish "this line will be reused in 50 ns" from "this line was used once and will not be touched for 500 ms." The polluting access wins the recency race and displaces the hot line.

### HFT Pollution Sources

**Logging on the critical path.** The most common and severe source. A trade execution path that writes to a memory-mapped log buffer (even with `O_DIRECT`) touches cache lines containing log record data, format strings, and timestamp buffers. If these share L1/L2 with the order book or risk check data structures, every log write evicts critical data.

Mitigation: route logging through `CLFLUSHOPT` + non-temporal stores (`_mm_stream_si64`) to write directly to DRAM without polluting the cache hierarchy:

```cpp
// Non-temporal store — bypasses cache entirely
_mm_stream_pi((__m64*)log_ptr, (__m64)log_data);
_mm_sfence();
```

**Control plane operations during trading.** Risk limit updates, strategy parameter changes, and configuration reloads issued by a risk manager on a separate thread share LLC with the trading engine. Even if risk updates are infrequent, the working set of the config/risk data can pollute L2/L3 lines occupied by hot order book state.

**Large memcpy and serialization.** Serializing a market data snapshot or risk report with `memcpy` or a protobuf encoder streams potentially megabytes through the cache, evicting the entire L2/L3 working set of the trading engine. This is the "big memcpy problem" — a 2 MB serialization pass fully evicts a 512 KB L2 and makes a large dent in a 16 MB L3. The subsequent trading decision incurs cold misses across all order book data.

**Instruction cache pollution.** Less frequently discussed but equally real: infrequently executed branches (error handling, connection teardown, kill-switch logic) whose instructions share I-cache sets with the hot path. After a network stack exception or watchdog check, I-cache lines for the hot-path decode logic may be displaced, adding decode-related latency stalls on the next market data event.

**Prefetcher pollution.** Aggressive hardware prefetchers detect sequential access patterns and prefetch speculatively. A background thread performing sequential scan of a large historical data structure can trigger the prefetcher to load irrelevant lines into L2/L3, polluting the cache with data the trading engine never requested.

### Architectural Mitigation: Cache Partitioning

Intel's **Cache Allocation Technology (CAT)**, part of the RDT (Resource Director Technology) suite, allows software to partition LLC ways among groups of cores. An HFT system can reserve 10 of 16 LLC ways exclusively for the trading engine's logical cores, preventing any other process (kernel, NIC driver, logger) from evicting trading-critical lines from those ways.

```
LLC Way Assignment (16-way, 32MB L3):
  Ways 0–9  → Trading Engine cores (CLOS 0)
  Ways 10–13 → Network/NIC cores (CLOS 1)
  Ways 14–15 → OS/kernel (CLOS 2)
```

This transforms LLC pollution from a probabilistic hazard into a structural guarantee.

---

## 4. Latency and Jitter Impact Analysis

### Latency vs. Jitter: A Critical Distinction

In HFT, **latency** (the mean or median end-to-end processing time) and **jitter** (the variance or tail latency, e.g., 99th/99.9th percentile) are distinct adversaries:

- **Latency** determines whether you win a race to a mispriced security.
- **Jitter** determines *reliability* — a strategy with 500 ns median but 50 µs P99.9 latency will occasionally be catastrophically slow, missing time-sensitive hedges or triggering stale-quote exposure.

Cache pathologies affect both, but in different ways and through different mechanisms.

### Latency Impact

| Pathology | Latency Mechanism | Typical Cost |
|---|---|---|
| L1 miss → L2 | Demand fetch, 3 cycles extra | ~1 ns |
| L2 miss → L3 | Demand fetch, 30 cycles extra | ~8 ns |
| L3 miss → DRAM | Full DRAM access, 200 cycles | ~60 ns |
| False sharing (HITM) | Inter-core transfer, 70–120 cycles | ~25–35 ns |
| Cache pollution (working set eviction) | Cascading L3 misses | 500 ns – 5 µs burst |

For a decision-to-order pipeline targeting sub-500 ns total latency, a single DRAM access (60 ns) consumes 12% of the budget. Two L3 misses in the critical path (order book lookup + risk check) consume 24%. False sharing on a hot path executed at 1 MHz generates 25–35 ns of inter-core latency per iteration, adding 25–35 µs of accumulated overhead per second.

### Jitter Impact

Cache pathologies are the **primary source of latency jitter** in a well-tuned HFT system (after eliminating OS scheduling noise and interrupt affinity issues). The mechanism is non-determinism in cache state:

**State-dependent miss rates.** Whether a cache line is present depends on the history of recent accesses by all threads on all cores sharing the LLC. This history is non-deterministic at the nanosecond scale. The same order processing path has L1-hit latency when the working set is warm and L3-miss latency when a transient burst of network traffic or logging evicted key lines moments before.

**Eviction timing non-determinism.** Pseudo-LRU replacement policies have implementation-dependent tie-breaking. Two executions with identical access sequences may evict different lines due to micro-architectural state (e.g., prefetcher state, prior branch predictor history). This creates irreducible jitter even in closed-loop benchmarks.

**NUMA effects under cross-socket access.** An HFT system may have NIC DMA writing market data into NUMA node 0 memory while the trading engine runs on NUMA node 1. Cross-socket coherence traffic introduces 100–200 ns latency with high variance depending on QPI/UPI link load — a major jitter source for remote market data reads.

**False sharing burst behavior.** False sharing latency is not uniformly distributed. Under light load, the coherence bus is uncongested and HITM penalties are consistent. Under bursty load (e.g., a volatility event causing 10× message rate), multiple cores simultaneously contend, queuing on the coherence protocol's arbitration layer. The HITM penalty becomes bimodal: fast in normal conditions, slow under contention — which is precisely when latency matters most.

### Compound Effects

The most dangerous scenario in HFT is **cascading cache degradation** under a market stress event. A volatility spike causes:

1. Message rate increases 10×, flooding the NIC ring buffer.
2. NIC DMA writes pollute the LLC with packet descriptor data.
3. The trading engine's order book data is partially evicted.
4. Increased message processing triggers more logging, further polluting L2/L3.
5. Risk limit checks on a wider instrument universe increase working set size beyond L2.
6. False sharing on the position counter (written by multiple strategy threads) spikes due to higher write frequency.

The result: latency during the stress event degrades from 300 ns (calm) to 15–50 µs (stressed) — exactly when sub-microsecond execution is most valuable. This pathological correlation between market opportunity magnitude and system latency degradation is the central systems challenge of HFT engineering.

### Quantifying the Jitter Floor

The minimum achievable jitter in an HFT system is bounded below by cache non-determinism. Even with CPU pinning, NUMA-local allocation, huge pages (eliminating TLB misses), and interrupt coalescing, the LLC's non-deterministic eviction policy introduces ~10–50 ns of irreducible jitter per cache line touched in the critical path. For a path touching 20 cache lines (order book, risk limits, position state, symbol metadata), the jitter floor is ~200–1000 ns — an absolute physical limit set by cache architecture, not software.

Overcoming this floor requires architectural changes: moving to FPGA-based execution (eliminating cache hierarchy entirely, operating from BRAM at ~1–5 ns deterministic latency) or kernel bypass with pre-pinned, pre-warmed memory mapped directly to trading logic.

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Describe cache pollution, false sharing, and cache misses in the high-frequency trading system in depth. This description is intended for a computer science expert. Analyze the impact of these mechanisms on latency and jitter.
