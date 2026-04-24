# AMD MOESI Cache-Coherency Protocol & HFT Implications

## 1. The MOESI Protocol — Core Mechanics

AMD CPUs implement **MOESI** (Modified, Owner, Shared, Exclusive, Invalid) — an extension of the classic MESI protocol. The key differentiator is the **Owner (O)** state, which AMD introduced to avoid mandatory writeback to main memory before sharing a dirty cache line.

### Cache Line States

| State | Dirty? | Other Caches May Hold? | Write Permission? |
|---|---|---|---|
| **M** odified | Yes | No | Yes |
| **O** wner | Yes | Yes (in S) | No |
| **E** xclusive | No | No | Yes (silent→M) |
| **S** hared | No | Yes | No |
| **I** nvalid | — | — | No |

### The MOESI State Machine

```
         Read Miss (no other copy)         Write (only copy)
              │                                  │
              ▼                                  ▼
           ┌─────┐   Write (sole owner)      ┌─────┐
   ──────► │  E  │ ─────────────────────►    │  M  │ ◄─────────────────┐
           └─────┘                           └─────┘                   │
              │                                 │                      │
    Remote    │  Read (another cache wants it)  │ Another cache        │
    Read      │                                 │ reads it             │ Write
    Hit       ▼                                 ▼                      │ (invalidate
           ┌─────┐                          ┌─────┐                    │  others)
           │  S  │ ◄──────────────────────  │  O  │ ───────────────────┘
           └─────┘   (dirty data shared;    └─────┘
              │       no writeback needed)     │
              │                                │
     Write    │                                │ Write
              ▼                                ▼
           ┌─────┐ ◄────────────────────── ┌─────┐
           │  M  │      (invalidate)        │  I  │
           └─────┘                          └─────┘
```

### The Owner State — AMD's Key Innovation

In MESI, when a **Modified** line must be shared, it must first be written back to LLC/RAM (M → S via memory). In MOESI:

- The dirty line transitions M → **O** on the original cache.
- Other caches receive the data directly (cache-to-cache transfer), entering **S**.
- The **Owner** cache is now responsible for future writebacks.
- **No memory round-trip required** — this is a major bandwidth and latency saver in multi-core workloads.

---

## 2. AMD's Multi-Die Topology: NUMA and the Infinity Fabric

Modern AMD EPYC/Ryzen processors (Zen 4/5) are **chiplet designs** — multiple Core Complex Dies (CCDs) connected via the **Infinity Fabric** (AMD's proprietary interconnect, essentially a high-speed serialized coherency bus).

```
┌─────────────────────────────────────────────────────────────────────┐
│                        AMD EPYC (Genoa, Zen 4)                      │
│                                                                     │
│  ┌────────────┐    ┌────────────┐    ┌────────────┐                 │
│  │   CCD 0    │    │   CCD 1    │    │   CCD 2    │  ...            │
│  │ ┌────────┐ │    │ ┌────────┐ │    │ ┌────────┐ │                 │
│  │ │ Core 0 │ │    │ │ Core 8 │ │    │ │Core 16 │ │                 │
│  │ │ L1 64K │ │    │ │ L1 64K │ │    │ │ L1 64K │ │                 │
│  │ │ L2  1M │ │    │ │ L2  1M │ │    │ │ L2  1M │ │                 │
│  │ └────────┘ │    │ └────────┘ │    │ └────────┘ │                 │
│  │    ...     │    │    ...     │    │    ...     │                 │
│  │ ┌────────┐ │    │ ┌────────┐ │    │ ┌────────┐ │                 │
│  │ │ L3 32M │ │    │ │ L3 32M │ │    │ │ L3 32M │ │                 │
│  │ │(shared)│ │    │ │(shared)│ │    │ │(shared)│ │                 │
│  └────────────┘    └────────────┘    └────────────┘                 │
│         │                │                │                         │
│         └────────────────┴────────────────┘                         │
│                          │                                          │
│                 ┌──────────────────┐                                │
│                 │  Infinity Fabric │  (coherency + data transport)  │
│                 │    IOD (I/O Die) │                                │
│                 │  Memory channels │                                │
│                 └──────────────────┘                                │
└─────────────────────────────────────────────────────────────────────┘
```

### Cache Hierarchy Latencies (approximate, Zen 4)

| Level | Size | Latency |
|---|---|---|
| L1-D | 32 KB / core | ~4 cycles |
| L2 | 1 MB / core | ~12 cycles |
| L3 (local CCD) | 32 MB / CCD | ~40 cycles |
| L3 (remote CCD, same die) | — | ~80–120 cycles |
| Remote NUMA node (cross-socket) | — | ~300–500 cycles |
| DRAM | — | ~200–300 cycles local; 500+ remote |

---

## 3. How MOESI Coherency Creates Latency and Jitter in HFT

### 3.1 False Sharing — The Silent Killer

False sharing occurs when two cores write to **different variables that reside on the same 64-byte cache line**. MOESI forces the line through M → I cycles between cores even though the logical data is independent.

```cpp
// PATHOLOGICAL: order_state and market_data share a cache line
struct SharedState {
    std::atomic<int64_t> order_state;   // written by order thread
    int64_t market_data;                 // written by feed thread
    // Both land in the same 64-byte cache line!
};
```

**Timeline under false sharing:**
```
T=0:   Core 0 writes order_state  → line is M on Core 0
T=1:   Core 1 writes market_data  → MOESI: sends RFO (Request For Ownership)
T=2:   Core 0 must invalidate     → line goes I on Core 0, M on Core 1
T=3:   Core 0 reads order_state   → cache miss, must fetch from Core 1
       (cross-core snoop + transfer: ~40–120 cycles)
```
This ping-pong can repeat thousands of times per second — in HFT terms, that's microseconds of unpredictable stall on the critical path.

### 3.2 MESI/MOESI Upgrade Storms — RFO Cascades

When a thread must write to a **Shared** line, it issues a **Read For Ownership (RFO)**, which:
1. Sends an invalidation snoop to all other caches holding the line.
2. Waits for acknowledgement from every sharing core (snoop filter must confirm all I transitions).
3. Only then grants exclusive ownership.

In a many-core AMD EPYC with 96+ cores, a popular shared structure (e.g., a lock-free queue head) can generate **snoop broadcast storms**:

```
Core 0 issues RFO → snoops sent to all CCDs via Infinity Fabric
                  → latency proportional to hop count × snoop ack latency
                  → non-deterministic depending on Infinity Fabric congestion
```

This is a primary source of **jitter** — the snoop completion time varies based on fabric utilisation.

### 3.3 Directory vs. Broadcast Snooping

Zen 4 uses a **combined** approach: a **snoop filter** (approximate directory) in the L3 slice tracks which cores have copies of each line. This avoids full broadcasts, but the filter itself introduces indirection latency, and filter evictions (capacity misses) force silent invalidations of lines that were being actively used — another source of jitter.

### 3.4 Cross-CCD and Cross-Socket Coherency

The Infinity Fabric operates at a configurable clock (FCLK, typically 1800–2000 MHz on Zen 4). Cross-CCD coherency requires:

1. Local L3 tag lookup
2. Infinity Fabric traversal to remote CCD
3. Remote tag lookup + snoop
4. Data or Ack returned via Fabric
5. Local integration

Each hop adds **~50–80 ns** of relatively deterministic latency, but fabric contention from other coherency traffic (other cores' snoops, PCIe DMA, memory prefetches) creates **non-deterministic jitter** — the nemesis of HFT.

### 3.5 NUMA Effects on Multi-Socket Systems

On 2-socket EPYC systems, the inter-socket coherency latency can exceed **300–500 ns** (several hundred cycles). If an HFT thread on Socket 0 reads a cache line last modified by a thread on Socket 1, MOESI must:

1. Detect the remote Owner/Modified state via the cross-socket directory.
2. Issue a cross-socket intervention request.
3. Transfer the cache line across the UPI/Infinity Fabric inter-socket link.

This is orders of magnitude more expensive than local L1 access, and is a **hard latency cliff** in HFT pipelines.

---

## 4. Mitigation Strategies

### 4.1 Eliminate False Sharing with Cache Line Padding

```cpp
// Align and pad to prevent false sharing
// Use hardware_destructive_interference_size (C++17)
#include <new>

struct alignas(std::hardware_destructive_interference_size) OrderState {
    std::atomic<int64_t> sequence{0};
    // Pad to fill the rest of the cache line
    char _pad[std::hardware_destructive_interference_size - sizeof(std::atomic<int64_t>)];
};

struct alignas(std::hardware_destructive_interference_size) MarketData {
    double bid;
    double ask;
    int64_t timestamp;
    char _pad[std::hardware_destructive_interference_size
              - 2 * sizeof(double) - sizeof(int64_t)];
};
```

For SPSC (single-producer, single-consumer) queues, pad the head and tail independently:

```cpp
template<typename T, size_t N>
struct SPSCQueue {
    alignas(64) std::atomic<size_t> head{0};
    char _pad0[64 - sizeof(std::atomic<size_t>)];

    alignas(64) std::atomic<size_t> tail{0};
    char _pad1[64 - sizeof(std::atomic<size_t>)];

    T buffer[N];
};
```

### 4.2 NUMA-Aware Thread and Memory Pinning

Bind threads to specific cores and allocate memory on the local NUMA node using `numactl` or programmatically:

```cpp
#include <numa.h>
#include <sched.h>

void pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

void* numa_local_alloc(size_t size) {
    int node = numa_node_of_cpu(sched_getcpu());
    return numa_alloc_onnode(size, node);
}
```

Use `libnuma` to ensure the critical path data structures (order books, ring buffers) are allocated on the same NUMA node as the threads that access them.

### 4.3 Use Seqlock for Read-Heavy Shared State

A **seqlock** allows concurrent readers without taking a write lock, and avoids the RFO storms of a mutex. Readers detect torn reads via the sequence counter:

```cpp
struct alignas(64) Seqlock {
    std::atomic<uint64_t> seq{0};

    void write_begin() {
        seq.fetch_add(1, std::memory_order_release); // odd = writing
    }
    void write_end() {
        seq.fetch_add(1, std::memory_order_release); // even = done
    }

    uint64_t read_begin() const {
        uint64_t s;
        do { s = seq.load(std::memory_order_acquire); }
        while (s & 1); // spin while write in progress
        return s;
    }

    bool read_retry(uint64_t s) const {
        return seq.load(std::memory_order_acquire) != s;
    }
};

// Usage (reader):
uint64_t s;
MarketSnapshot snap;
do {
    s = lock.read_begin();
    snap = *shared_snapshot; // potentially torn read
} while (lock.read_retry(s)); // retry if writer intervened
```

This keeps the data line in **S** state on all readers simultaneously — no RFO, no MOESI invalidation cascade.

### 4.4 Prefer Atomic Loads with Relaxed Ordering on the Hot Path

Not every atomic needs sequential consistency. Use the weakest ordering that maintains correctness:

```cpp
// Writer (single producer):
sequence.store(new_seq, std::memory_order_release);

// Reader (single consumer):
uint64_t s = sequence.load(std::memory_order_acquire);
// acquire/release pair: no full fence, no mfence instruction
// avoids pipeline stall while still providing happens-before
```

Avoid `std::memory_order_seq_cst` on the hot path — it compiles to `LOCK XCHG` or `MFENCE` on x86-64, which flushes the store buffer and stalls the pipeline.

### 4.5 Prefetching to Hide Coherency Latency

Issue software prefetches ahead of time so the MOESI state transition completes before the data is needed:

```cpp
// Prefetch next message while processing current one
__builtin_prefetch(&ring_buffer[(head + 1) & mask], 0, 3); // read, high locality

// For write prefetch (takes ownership early, issues RFO ahead of time):
__builtin_prefetch(&output_slot, 1, 3); // write, high locality
```

The write prefetch (`_MM_HINT_ET0` / locality=1 in the prefetch intrinsic) issues the **RFO early**, so by the time the actual store executes, the cache line is already in **E** or **M** state and the write completes in a single cycle.

### 4.6 Isolate CPUs from the OS Scheduler

Use Linux kernel parameters to dedicate cores entirely to HFT threads:

```bash
# In /etc/default/grub:
GRUB_CMDLINE_LINUX="isolcpus=2-15 nohz_full=2-15 rcu_nocbs=2-15 \
                    intel_pstate=disable processor.max_cstate=1 \
                    idle=poll nosoftlockup"
```

Then use `taskset` or `cpuset` cgroups. The `nohz_full` parameter disables the periodic timer tick on isolated cores, eliminating **jitter from timer interrupts** that would otherwise disturb cache state by pulling OS code into the L1/L2.

### 4.7 Huge Pages to Reduce TLB Pressure

TLB misses force page-table walks, which are themselves subject to MOESI coherency (page table entries are shared memory). Use **2 MB huge pages** for large data structures:

```cpp
#include <sys/mman.h>

void* alloc_hugepage(size_t size) {
    void* ptr = mmap(nullptr, size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                     -1, 0);
    if (ptr == MAP_FAILED) {
        // fallback: madvise(MADV_HUGEPAGE) on regular allocation
        ptr = aligned_alloc(4096, size);
        madvise(ptr, size, MADV_HUGEPAGE);
    }
    return ptr;
}
```

With 4 KB pages, a 1 GB order book needs 262,144 TLB entries. With 2 MB pages, it needs only 512 — drastically reducing coherency traffic on the page-table walker's shared structures.

### 4.8 Core-Local Data Structures — Avoid Sharing Altogether

The most effective mitigation is **eliminating coherency traffic** entirely by partitioning data per-core:

```cpp
// Each core has its own slab — no sharing, no coherency overhead
struct alignas(4096) PerCoreState {
    OrderBook local_book;
    RiskLimits local_limits;
    uint64_t order_counter;
    char _pad[4096 - (sizeof(OrderBook) + sizeof(RiskLimits) + sizeof(uint64_t)) % 4096];
};

// Indexed by core ID — each core only touches its own slot
PerCoreState __attribute__((aligned(4096))) per_core[MAX_CORES];

// Access pattern: zero coherency traffic
PerCoreState& my_state = per_core[sched_getcpu()];
```

Use a **fanout/merge** pattern: cores maintain local state and periodically synchronise with a designated aggregator core using a lockless SPSC channel. This replaces many-to-many coherency with point-to-point messages.

---

## 5. Summary — Latency Budget and Mitigations at a Glance

| Problem | MOESI Mechanism | Mitigation | Latency Saved |
|---|---|---|---|
| False sharing | M→I ping-pong | Cache line padding / alignment | 40–120 cycles |
| Shared write contention | RFO broadcast storms | Seqlock / per-core data | 100–500 cycles |
| Cross-CCD access | Infinity Fabric hop | NUMA pinning / local alloc | 80–200 cycles |
| Cross-socket | Dir. coherency | Single-socket topology | 300–800 cycles |
| OS jitter | Scheduler preemption disturbs L1 | `isolcpus` / `nohz_full` | microseconds of jitter |
| TLB miss → coherency | Page-table walk | Huge pages | 10–50 cycles |
| Late ownership | Write hits S/I line | Write prefetch (RFO early) | 40–120 cycles |

The overriding principle in HFT cache design is: **own the cache line exclusively before you need it, keep data writer-local, and treat any cross-core sharing as a design flaw to be eliminated** — not merely optimised.

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Describe the cache-coherency protocol available on modern CPUs provided by AMD in detail. Explain how the protocol affects latency and jitter in high-frequency trading systems. Provide hints how to mitigate latency and jitter in the HFT sysstems. This description is intended for a computer science expert who knows the C++ language.
