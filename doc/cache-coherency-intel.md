# Cache Coherency on Modern Intel CPUs and Its Impact on HFT Systems

---

## 1. The MESIF Protocol

Modern Intel CPUs implement **MESIF** (an extension of MESI), a directory-assisted, snooping-based cache coherency protocol. Each cache line (64 bytes on all modern Intel chips) exists in exactly one of five states across the entire system at any given moment.

| State | Meaning | Can Read? | Can Write? | Shareable? |
|---|---|---|---|---|
| **M**odified | Dirty, exclusive local copy | ✅ | ✅ | ❌ |
| **E**xclusive | Clean, exclusive local copy | ✅ | ✅ (silent → M) | ❌ |
| **S**hared | Clean, potentially multi-copy | ✅ | ❌ | ✅ |
| **I**nvalid | Line not present / stale | ❌ | ❌ | N/A |
| **F**orward | Designated responder among sharers | ✅ | ❌ | ✅ |

The **F** state is Intel's key extension: when multiple cores share a line, exactly one is designated the *forwarder*. On a read request from a new core, the forwarder supplies the data directly (core-to-core), avoiding a round-trip to main memory. This is critical for LLC (Last Level Cache) hit latency.

---

## 2. The Physical Interconnect: Ring Bus vs. Mesh

### Ring Bus (Skylake and earlier, still common in desktop/HEDT)
All cores, LLC slices, memory controllers, and I/O agents sit on a single bidirectional ring. Coherency traffic is broadcast-snooped around the ring. Latency scales **O(N)** with core count and hop distance.

### Mesh (Skylake-SP / Cascade Lake / Ice Lake-SP / Sapphire Rapids — server)
Cores are arranged in a 2D grid. Each core has a **Home Agent (HA)** that owns a slice of the distributed LLC and acts as the directory for lines that hash to it. The **Caching Agent (CA)** on each core handles local cache operations. Coherency is **directory-based** rather than pure snooping:

```
Core A (CA) ──── Mesh Stop ──── Core B (CA)
                     │
               Home Agent (HA)
               (owns LLC slice + directory)
```

A write from Core A to a line homed at Core B's HA involves:

1. CA_A sends `RFO` (Read For Ownership) to HA_B
2. HA_B checks its directory: finds line in S-state on Core C
3. HA_B sends `INVL` snoop to CA_C
4. CA_C sends `ACK` + transitions to I
5. HA_B grants ownership to CA_A
6. CA_A transitions line to M, writes

Each arrow is a mesh hop; hop latency is ~5–10 ns per hop on a 32-core Sapphire Rapids socket.

---

## 3. Key Latency Numbers (Approximate, Intel Sapphire Rapids / Alder Lake)

| Access Path | Latency |
|---|---|
| L1 hit | ~4–5 cycles (~1.5 ns @ 3.5 GHz) |
| L2 hit | ~12–14 cycles (~4 ns) |
| L3 hit (local slice) | ~30–40 cycles (~10 ns) |
| L3 hit (remote slice, same socket) | ~50–80 cycles (~20 ns) |
| Core-to-core (MESIF forward, same socket) | ~40–80 cycles |
| Remote socket (NUMA, QPI/UPI) | ~100–200 ns |
| DRAM (local) | ~60–80 ns |
| DRAM (remote NUMA node) | ~120–160 ns |

---

## 4. How MESIF Creates Latency and Jitter in HFT

### 4.1 False Sharing
Two threads on different cores write to *different* variables that occupy the *same* 64-byte cache line. Every write by either thread forces a full RFO cycle on the line, serializing what should be independent operations.

```cpp
// DANGER: x and y share a cache line
struct alignas(8) BadCounters {
    std::atomic<uint64_t> x;  // offset 0
    std::atomic<uint64_t> y;  // offset 8 — same line!
};

// FIX: pad to separate cache lines
struct alignas(64) GoodCounters {
    std::atomic<uint64_t> x;
    char pad[56];
};
// or use hardware_destructive_interference_size (C++17):
alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> x;
alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> y;
```

### 4.2 True Sharing (Producer-Consumer Coherency Storms)
A lock-free SPSC queue's head/tail pointers, if written by both producer and consumer, generate continuous RFO traffic. Even when separated into different cache lines, the *data* lines themselves transition M→S→M on every round-trip.

### 4.3 NUMA Effects
On multi-socket systems, if the OS scheduler migrates a thread from socket 0 to socket 1, all its working-set lines become remote NUMA accesses — a 2–3× latency penalty that appears as a jitter spike, not a sustained overhead.

### 4.4 LLC Thrashing and Eviction
On a 32-core mesh, LLC slices are distributed. A hash of the physical address determines the home HA. A cache miss that goes to a non-local LLC slice incurs extra mesh hops. Under load, conflict evictions to DRAM introduce unpredictable multi-microsecond latency spikes.

### 4.5 Memory Ordering and Store Buffers
Intel uses **TSO** (Total Store Order). Stores are buffered in the per-core store buffer and become visible to other cores only after draining (i.e., after `MFENCE` or implicit serialization from `LOCK`-prefixed instructions). An `std::atomic` store with `memory_order_seq_cst` compiles to a `LOCK XCHG` or `MFENCE`, flushing the store buffer — this is expensive (~20–40 cycles) and introduces jitter under contention.

```cpp
// seq_cst: ~20-40 cycle store buffer drain
order_book.best_bid.store(new_price, std::memory_order_seq_cst);

// release: just a compiler fence on x86 TSO — much cheaper
order_book.best_bid.store(new_price, std::memory_order_release);
// safe on x86 because TSO provides implicit acquire-release for loads/stores
```

---

## 5. Mitigation Strategies

### 5.1 CPU and NUMA Pinning
Eliminate OS scheduler jitter entirely.

```cpp
// Pin thread to core 3
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(3, &cpuset);
pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

// Allocate memory on the local NUMA node
void* buf = numa_alloc_onnode(size, numa_node_of_cpu(3));
```

Use `numactl --membind=0 --cpunodebind=0 ./hft_app` at launch. Never let the OS decide.

### 5.2 Isolate HFT Cores with `isolcpus`
Remove cores from the kernel's scheduler entirely:

```
# /etc/default/grub
GRUB_CMDLINE_LINUX="isolcpus=2-15 nohz_full=2-15 rcu_nocbs=2-15"
```

`nohz_full` disables the scheduling-clock interrupt on isolated cores, eliminating ~1 µs periodic jitter. `rcu_nocbs` offloads RCU callbacks off hot cores.

### 5.3 Cache Line Discipline

**Separate hot read-only data from mutable state:**
```cpp
// Read-only market data: packed for cache density
struct alignas(64) MarketSnapshot {
    int64_t  bid_price;
    int64_t  ask_price;
    uint32_t bid_size;
    uint32_t ask_size;
    uint64_t seq_no;
    // fits in one cache line: 32 bytes
};

// Mutable per-core state: padded to prevent false sharing
struct alignas(64) CoreState {
    uint64_t order_count;
    uint64_t fill_count;
    char     _pad[48];
};
static CoreState per_core_state[MAX_CORES];
```

**Prefetch before you need it:**
```cpp
// While processing message N, prefetch message N+1's cache line
__builtin_prefetch(next_msg_ptr, 0, 1);  // read, low temporal locality
```

### 5.4 Lock-Free Data Structures with Coherency Awareness

A cache-friendly SPSC ring buffer: keep `head` and `tail` on separate cache lines, and write data *before* advancing the index so the consumer never sees a partially-written entry.

```cpp
template<typename T, size_t N>
class SPSCQueue {
    static_assert((N & (N-1)) == 0, "N must be power of 2");

    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};  // separate cache line
    alignas(64) T ring_[N];

public:
    bool push(const T& val) noexcept {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) + N <= t)
            return false;  // full
        ring_[t & (N-1)] = val;
        // release: makes the write to ring_ visible before tail advance
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& val) noexcept {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        if (tail_.load(std::memory_order_acquire) <= h)
            return false;  // empty
        val = ring_[h & (N-1)];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
};
```

Note the deliberate use of `memory_order_relaxed` / `acquire` / `release` rather than `seq_cst` — on x86 TSO this avoids `MFENCE` while still being correct.

### 5.5 Huge Pages to Reduce TLB Pressure

TLB misses cause page-table walks which are effectively cache misses on PTEs — another source of jitter.

```cpp
// 2 MB huge pages via mmap
void* buf = mmap(nullptr, size,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                 -1, 0);

// Or configure at OS level:
// echo 512 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
// Then use MAP_HUGETLB or hugetlbfs mount
```

### 5.6 Avoid RFO Storms: Write Combining and Non-Temporal Stores

For outbound packet buffers that are written-then-DMA'd (never read back by the CPU), bypass the cache entirely:

```cpp
#include <immintrin.h>

void write_packet_nocache(void* dst, const void* src, size_t len) {
    // Uses MOVNTDQ — non-temporal store, bypasses LLC
    // Requires dst to be 16-byte aligned; len multiple of 16
    const __m128i* s = reinterpret_cast<const __m128i*>(src);
    __m128i*       d = reinterpret_cast<__m128i*>(dst);
    for (size_t i = 0; i < len / 16; ++i)
        _mm_stream_si128(d + i, _mm_load_si128(s + i));
    _mm_sfence();  // ensure stores are visible before DMA
}
```

This avoids the MESIF RFO entirely for write-only buffers, freeing LLC bandwidth for hot read paths.

### 5.7 DDIO Awareness (Intel Data Direct I/O)

On Xeon server CPUs, **DDIO** allows NICs to DMA incoming packets directly into the LLC rather than DRAM. This is extremely valuable for HFT:

- Incoming market data lands in LLC (~10 ns) rather than DRAM (~70 ns)
- The receiving core finds the packet in L3 on first touch

Ensure DDIO is enabled (it is by default on Xeon since Ivy Bridge-EP) and that your packet-processing cores share LLC slices with the I/O hub. You can tune DDIO's LLC allocation with Intel's **Resource Director Technology (RDT)** / **Cache Allocation Technology (CAT)**:

```bash
# Reserve LLC ways 0-3 exclusively for NIC DMA (DDIO), ways 4-11 for HFT cores
pqos -e "llc:0=0x00f;llc:1=0xff0"   # using intel-cmt-cat tools
```

### 5.8 Spin-Wait with PAUSE and Back-off

Tight spin loops without `PAUSE` cause the CPU's pipeline to misprediction-flood the branch predictor and can delay coherency updates (the core monopolizes the memory bus):

```cpp
// Bad: burns pipeline, delays coherency
while (!flag.load(std::memory_order_acquire)) {}

// Good: PAUSE hints the pipeline, reduces power, improves coherency observation latency
while (!flag.load(std::memory_order_acquire)) {
    _mm_pause();  // compiles to PAUSE instruction
}
```

On Skylake+, `PAUSE` has been extended to ~140 cycles of de-pipeline delay (up from ~5 on earlier µarchs), significantly reducing power and bus pressure during spin-waits.

---

## 6. Summary Decision Matrix

| Problem | Root Cause | Fix |
|---|---|---|
| Periodic µs jitter | OS scheduler, clock IRQ | `isolcpus`, `nohz_full`, thread pinning |
| High latency on shared data | MESIF RFO chain | Lock-free structures, relaxed atomics |
| False sharing | Cache line straddling | `alignas(64)`, padding, `hardware_destructive_interference_size` |
| NUMA penalty | Remote socket access | `numa_alloc_onnode`, socket pinning |
| TLB misses | 4K page table walks | 2MB/1GB huge pages |
| LLC eviction jitter | Working set > LLC | CAT/RDT partitioning, data layout |
| Write-only buffer RFOs | Cache pollution | Non-temporal stores (`_mm_stream_si128`) |
| NIC → CPU latency | DRAM DMA path | DDIO + CAT LLC reservation |

The overarching principle is **locality of reference in both space and time**: keep hot data on the core that owns it, prevent coherency state transitions on the critical path, and eliminate every source of non-determinism the OS and hardware would otherwise introduce.

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Describe the cache-coherency protocol available on modern CPUs provided by Intel in detail. Explain how the protocol affects latency and jitter in high-frequency trading systems. Provide hints how to mitigate latency and jitter in the HFT sysstems. This description is intended for a computer science expert who knows the C++ language.
