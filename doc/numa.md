# Non-Uniform Memory Access (NUMA) Architecture

---

## 1. Architectural Overview

Modern servers rarely feature a single monolithic processor. Instead, they house multiple **physical processor packages** (sockets), each containing several CPU cores, private caches, and — crucially — a portion of the total system RAM soldered or connected directly to that socket. This is the defining feature of NUMA: **memory access latency is not uniform** across all memory locations because it depends on the physical relationship between the requesting core and the target memory bank.

Each processor package, together with its locally attached memory and interconnect logic, forms a **NUMA node**. The operating system exposes this topology through abstractions like `libnuma` on Linux or `NUMA APIs` on Windows, but the hardware reality underneath is what drives every performance consequence you will encounter.

### The Memory Hierarchy in a NUMA System

```
Socket 0                              Socket 1
┌─────────────────────────────────┐  ┌─────────────────────────────────┐
│  Core 0  Core 1  Core 2  Core 3 │  │  Core 4  Core 5  Core 6  Core 7 │
│   L1/L2   L1/L2   L1/L2  L1/L2  │  │   L1/L2   L1/L2   L1/L2  L1/L2  │
│         Shared L3 Cache         │  │         Shared L3 Cache         │
│        Memory Controller        │  │        Memory Controller        │
│         Local DRAM (e.g. 64GB)  │  │         Local DRAM (e.g. 64GB)  │
└────────────┬────────────────────┘  └──────────────┬──────────────────┘
             │                                      │
             └──────────── Interconnect ────────────┘
                     (QPI / UPI / AMD IF)
```

**Local memory access** (Core 0 → Socket 0 DRAM): ~60–80 ns.  
**Remote memory access** (Core 0 → Socket 1 DRAM): ~120–160 ns — often 1.5–2× slower, sometimes more on 4- and 8-socket systems.

On AMD EPYC processors, the complexity deepens further. Each socket is itself composed of multiple **chiplets (CCDs)**, each with its own local L3 cache and an internal fabric interconnect (Infinity Fabric). This creates a **sub-NUMA clustering** effect — even within a single socket, accessing memory attached to a different CCD incurs additional latency hops.

---

## 2. Interconnect Technology

The physical bridge between NUMA nodes is a high-speed point-to-point interconnect:

- **Intel QPI (QuickPath Interconnect)** / **UPI (Ultra Path Interconnect)**: A packetized, cache-coherent link operating at 9.6–16 GT/s per direction, with a full-width transfer of 8 bytes. UPI (used since Skylake-SP) improves bandwidth and latency over QPI but is still measurably slower than local memory access.
- **AMD Infinity Fabric (GMI/xGMI)**: Used both within an EPYC socket (inter-CCD) and between sockets (inter-socket). Its bandwidth scales with clock frequency, and AMD allows tuning it relative to memory speed.
- **IBM POWER**: Uses a proprietary high-bandwidth interconnect with hardware-managed cache coherence at the interconnect level.

These links carry **cache-coherency traffic** — not just plain data reads and writes, but also snoops, invalidations, and acknowledgments as part of a distributed cache-coherence protocol (typically MESIF or MOESI variants). Every remote cache line access involves this protocol, which is a key source of latency overhead.

---

## 3. Cache Coherence Across NUMA Nodes

Modern NUMA systems implement **directory-based cache coherence**, an evolution beyond bus-snooping that scales to multiple nodes. Each cache line has a **home node** — the NUMA node whose memory controller "owns" the directory entry for that line.

A remote read from Core 0 (Socket 0) of a cache line homed on Socket 1 proceeds roughly as:

1. Core 0 issues a load; it misses L1, L2, and L3.
2. A request traverses the UPI/IF to the **home node** (Socket 1).
3. Socket 1's directory controller checks if any other node has the line in a Modified or Exclusive state.
4. If clean: the data travels back over the interconnect to Core 0's L3/L2/L1.
5. If dirty (owned by Socket 0's L3): an **intervention** occurs — Socket 1 asks Socket 0 to supply the line, adding another hop.

This multi-hop protocol is why remote access is so expensive: it's not merely the raw bandwidth bottleneck, but the **round-trip latency** of coherence messages traveling at the speed of electrical signals across centimeters of silicon and PCB traces.

---

## 4. Impact on Multithreaded Application Performance

### 4.1 False Sharing Amplified

False sharing — multiple threads writing to different variables that share a cache line — is painful on any SMP system. On NUMA, it's catastrophic. Each write from a remote-node core triggers:

- A coherence invalidation crossing the interconnect.
- A subsequent remote fetch to re-acquire the line.
- Additional directory traffic to track ownership.

What costs ~10 ns on a UMA system (local L3 bounce) can cost ~150–300 ns on NUMA when the line bounces between sockets.

```cpp
// Classic false sharing – devastating on NUMA
struct alignas(8) Counter { int64_t value; };
Counter counters[NUM_THREADS]; // Packed: multiple counters per cache line

// Fix: pad to cache line boundary
struct alignas(64) PaddedCounter { int64_t value; };
PaddedCounter counters[NUM_THREADS]; // Each counter owns its cache line
```

### 4.2 Remote Memory Bandwidth Saturation

The inter-socket interconnect has a finite aggregate bandwidth — typically 40–100 GB/s depending on the platform, which is shared among all cross-socket traffic. A bandwidth-bound workload (e.g., a streaming computation over a large array) will saturate the interconnect long before it saturates local DRAM bandwidth, leading to stalls. Profiling tools like Intel VTune report this as high "Remote DRAM" or "Inter-Socket Bandwidth" utilization.

### 4.3 Thread Migration

The OS scheduler, if not NUMA-aware, will migrate threads between cores freely. A thread migrated from Socket 0 to Socket 1 suddenly faces remote latency for all the data its working set touched locally. Linux's CFS scheduler has NUMA balancing heuristics (`/proc/sys/kernel/numa_balancing`), but these are reactive and imperfect — they incur page migration costs while they adapt.

### 4.4 Memory Allocation Policy

The default Linux allocator follows a **first-touch policy**: a virtual page is physically backed by the NUMA node of the thread that first writes to it. This is rational but fragile. If an initializer thread on Node 0 touches all pages of a large buffer, and worker threads on Node 1 then process it, those workers suffer remote latency for the entire computation.

### 4.5 Lock Contention Across Nodes

A mutex implemented with a single shared cache line (as `std::mutex` typically is) becomes a hot cross-NUMA resource when threads on multiple nodes compete for it. Each acquisition/release bounces the lock line across the interconnect, serializing not just the critical section but also the coherence protocol itself.

---

## 5. Methods to Improve Performance

### 5.1 NUMA-Aware Memory Allocation

Allocate memory on the node where it will be primarily accessed. On Linux:

```cpp
#include <numaif.h>
#include <numa.h>

// Allocate memory local to node 0
void* ptr = numa_alloc_onnode(size_bytes, /*node=*/0);

// Or use mbind() for finer control on an existing allocation
void* buf = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
unsigned long nodemask = 1UL << target_node;
mbind(buf, size, MPOL_BIND, &nodemask, sizeof(nodemask) * 8, 0);
```

For C++ allocators, you can wrap `numa_alloc_onnode` / `numa_free` in a custom `std::pmr::memory_resource` or a stateful allocator and pass it to containers:

```cpp
template<typename T>
struct NumaAllocator {
    using value_type = T;
    int node;
    explicit NumaAllocator(int node) : node(node) {}

    T* allocate(std::size_t n) {
        void* p = numa_alloc_onnode(n * sizeof(T), node);
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t n) {
        numa_free(p, n * sizeof(T));
    }
};

std::vector<int, NumaAllocator<int>> local_vec(NumaAllocator<int>(my_node));
```

### 5.2 Thread Pinning (CPU Affinity)

Pin threads to specific cores, and ensure they access only locally allocated data. This prevents the scheduler from migrating threads and makes memory locality guarantees reliable:

```cpp
#include <pthread.h>
#include <sched.h>

void pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}
```

Use `numactl --cpunodebind=0 --membind=0 ./my_app` at launch for coarse-grained binding without recompiling.

### 5.3 Data Partitioning and Replication

Partition large datasets so that each NUMA node owns the portion of data its threads will process. For read-heavy shared data, **replicate** it across all nodes rather than sharing one copy:

```cpp
// Instead of one global read-mostly config:
struct Config { /* ... */ };
Config global_config; // All nodes read this — remote traffic for non-node-0 threads

// Replicate per-node:
std::vector<Config> per_node_configs(numa_num_configured_nodes(), global_config);
// Each thread reads per_node_configs[my_node_id]
```

### 5.4 NUMA-Aware Thread Pool Design

Design thread pools that are partitioned by NUMA node. Each node has its own sub-pool with its own task queue. Tasks submitted to the pool are tagged with a preferred node, and the scheduler assigns them to a thread on that node. This is the architecture used in frameworks like Intel TBB's `task_arena` with NUMA-awareness and `hwloc`.

### 5.5 Eliminating False Sharing

Use `alignas(std::hardware_destructive_interference_size)` (C++17) to pad hot per-thread data to a full cache line:

```cpp
#include <new> // for hardware_destructive_interference_size

struct alignas(std::hardware_destructive_interference_size) PerThreadData {
    std::atomic<int64_t> counter{0};
    // pad to 64 bytes to prevent sharing
};

PerThreadData thread_data[MAX_THREADS];
```

### 5.6 Reducing Lock Contention: MCS and Hierarchical Locks

Standard spinlocks create excessive interconnect traffic. **MCS locks** (Mellor-Crummey Scott) construct a per-thread node in a queue, so each waiting thread spins on its own local memory rather than on the shared lock word:

```cpp
struct MCSNode {
    std::atomic<MCSNode*> next{nullptr};
    std::atomic<bool> locked{true};
};

// Acquire: atomically append this thread's node to the queue
// Spin on node.locked (local memory) rather than the global lock word
```

For NUMA, **hierarchical locks** go further: they favor threads on the same node, batching local acquisitions before passing the lock to a remote node, dramatically reducing interconnect traffic under contention.

### 5.7 First-Touch Initialization

Initialize data on the thread that will use it, not on a dedicated initializer thread:

```cpp
// BAD: main thread (likely on node 0) touches all pages
std::vector<double> data(N);
std::fill(data.begin(), data.end(), 0.0);
// Now hand off to worker threads on all nodes – they see remote memory

// GOOD: parallel initialization, each thread touches its own partition
#pragma omp parallel for schedule(static)
for (int i = 0; i < N; ++i)
    data[i] = 0.0; // Each thread first-touches its own pages
```

### 5.8 Topology Discovery with hwloc

The `hwloc` library provides a portable, precise topology map:

```cpp
#include <hwloc.h>

hwloc_topology_t topology;
hwloc_topology_init(&topology);
hwloc_topology_load(topology);

// Find the number of NUMA nodes
int n_nodes = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_NUMANODE);

// Get the NUMA node closest to the current thread's core
hwloc_cpuset_t current = hwloc_bitmap_alloc();
hwloc_get_last_cpu_location(topology, current, HWLOC_CPUBIND_THREAD);
hwloc_obj_t node = hwloc_get_numanode_obj_by_os_index(topology, /*...*/);

hwloc_topology_destroy(topology);
```

This lets you programmatically build NUMA-aware work distribution without hardcoding node counts.

---

## 6. Communication Flow Between CPU Components

Here is a step-by-step trace of how data moves through the hardware on a 2-socket NUMA system for a representative scenario: **Core A (Socket 0) reads a cache line that Core B (Socket 1) has recently written and still holds in Modified state.**

```
Step 1: Core A issues a load to virtual address V.
        TLB translates V → Physical Address P.
        P hashes to a home node: Socket 1.

Step 2: Core A's L1 cache → MISS
        Core A's L2 cache → MISS
        Core A's L3 cache → MISS (no copy anywhere in Socket 0)

Step 3: Socket 0's Caching/Home Agent (CHA) issues a
        "Read Request" packet over UPI toward Socket 1.

Step 4: Socket 1's Home Agent receives the request.
        It checks its **directory**: P is in state M, owned by Core B's L1.

Step 5: Socket 1's HA sends a "Snoop Invalidate" to Core B.
        Core B's L1 flushes the modified line → writes it back to
        Socket 1's DRAM (or directly forwards it – depends on
        whether the platform supports "forward" optimizations).

Step 6: The cache line data travels:
        Core B's L1 → Socket 1's HA → UPI link → Socket 1's HA
        confirms write-back. Data packet travels over UPI to Socket 0.

Step 7: Socket 0's HA places the line into Core A's L3,
        then L2, then L1. The original load instruction retires.

Total: 2–4 UPI round trips, ~120–200 ns end-to-end.
       Compare to ~5 ns for an L1 hit or ~60 ns for a local DRAM fetch.
```

This coherence dance is the physical reason why cross-socket sharing of mutable data is so expensive. The UPI/IF links are not just slow pipes — they carry **protocol messages** (requests, snoops, acknowledgments, data responses) that serialize in the directory and add queuing delay under load.

### Internal Socket Communication (Within a Node)

Within a single socket, cores communicate via the **ring bus** (Intel pre-Skylake) or a **mesh interconnect** (Intel Skylake-SP and later, AMD EPYC). On a mesh, each tile (core + L3 slice + directory) connects to adjacent tiles. Coherence within the socket still requires cross-tile messages if a core accesses an L3 slice not mapped to its local tile, but these are sub-microsecond and don't traverse the UPI link.

---

## 7. Tooling and Profiling

| Tool | Purpose |
|---|---|
| `numactl --hardware` | Display NUMA topology and distance table |
| `lstopo` (hwloc) | Visual topology map |
| Intel VTune Profiler | Remote DRAM access rate, UPI bandwidth utilization |
| `perf stat -e numa-*` | Hardware PMU NUMA events |
| `numastat` | Per-node allocation statistics for a running process |
| AMD uProf | NUMA analysis for EPYC platforms |
| `perf c2c` | Detect false sharing and cache-line contention |

The `perf c2c` (cache-to-cache) tool deserves special mention: it uses PEBS/IBS hardware sampling to identify specific cache lines that are experiencing cross-node bouncing, giving you the exact struct field or array element responsible.

---

## Summary

NUMA is fundamentally a scaling solution that trades the simplicity of uniform memory for the raw scalability of distributed, multi-socket memory pools. The performance hazards it introduces — remote latency, interconnect saturation, coherence storms from false sharing, and policy-sensitive memory placement — are all consequences of a single physical reality: there is no free lunch in spanning silicon across multiple packages with a shared address space. For the C++ practitioner, mastering NUMA means treating memory not as a flat array but as a **topology**: partition data by node, pin threads to cores, initialize data where it will live, keep mutable state strictly local, and instrument everything with hardware PMU counters to verify that your assumptions about locality are actually reflected in the silicon.

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Provide a thorough description of Non-Uniform Memory Access (NUMA) computer architecture. Evaluate the influence of NUMA on the performance of multithreaded application. Provide the methods to increase the performance of the application. Discuss how the communication flows between various parts of the CPU with NUMA. This description is intended for a computer science expert familiar with C++ language. 
