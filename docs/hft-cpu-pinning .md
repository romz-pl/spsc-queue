# CPU Pinning in High-Frequency Trading Systems

## 1. Foundational Concepts

### What CPU Pinning Is

CPU pinning — also called **processor affinity** or **CPU affinity** — is the explicit binding of a software thread (or process) to a specific logical CPU core (or set of cores), preventing the OS scheduler from migrating it to another core. In Linux, this is expressed via the `sched_setaffinity(2)` syscall, which manipulates a `cpu_set_t` bitmask. In user space, the POSIX wrapper `pthread_setaffinity_np()` achieves the same effect per-thread.

```c
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(core_id, &cpuset);
pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
```

In HFT, this is never used in isolation — it is the cornerstone of a broader **CPU isolation and partitioning strategy**.

---

## 2. The HFT Threading Architecture

A production HFT system partitions cores into three zones:

```
┌─────────────────────────────────────────────────────────────────────┐
│                         NUMA Node 0                                 │
│                                                                     │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐   │
│  │  OS / System │  │  Ancillary   │  │   Latency-Critical Zone  │   │
│  │  Zone        │  │  Zone        │  │                          │   │
│  │              │  │              │  │  Core 4: NIC IRQ         │   │
│  │  Cores 0-1   │  │  Cores 2-3   │  │  Core 5: Kernel Bypass   │   │
│  │  (housekeep) │  │  (logging,   │  │  Core 6: Strategy Engine │   │
│  │              │  │   risk)      │  │  Core 7: Order Gateway   │   │
│  └──────────────┘  └──────────────┘  └──────────────────────────┘   │
│                                                                     │
│            L3 Cache (shared across all cores)                       │
└─────────────────────────────────────────────────────────────────────┘
```

**Key architectural decisions:**

- **NIC interrupt affinity** is pinned to a dedicated core using `/proc/irq/<N>/smp_affinity_list` so that the NAPI softirq processing happens on a known, isolated core.
- **The hot path** (market data → strategy → order) threads are pinned to adjacent cores sharing an L2 or L3 cache slice to exploit cache locality.
- **Ancillary threads** (risk checks, logging, persistence) are affined to a separate set of cores to prevent cache pollution of the hot path.

---

## 3. The Linux Isolation Stack Required to Make Pinning Effective

CPU pinning alone is **insufficient**. Without the following complementary mechanisms, rogue OS activity will still cause jitter:

### 3.1 `isolcpus` Kernel Boot Parameter
```
isolcpus=4,5,6,7 nohz_full=4,5,6,7 rcu_nocbs=4,5,6,7
```
- `isolcpus`: Removes the listed cores from the general scheduler domain. The kernel will not schedule any task there unless explicitly asked via affinity.
- `nohz_full`: Suppresses the periodic scheduler tick (normally 250 Hz or 1000 Hz) on isolated cores when only one runnable task is present. This eliminates **tick-induced jitter** of 1–4 µs every 1–4 ms.
- `rcu_nocbs`: Offloads RCU (Read-Copy-Update) callbacks from isolated cores to housekeeping cores, removing another source of asynchronous interrupts.

### 3.2 IRQ Balancing Suppression
The `irqbalance` daemon must be **stopped** (or configured to exclude isolated cores), otherwise it will opportunistically migrate hardware interrupts onto the pinned cores.

### 3.3 Memory Pinning and NUMA Locality
CPU pinning is paired with memory pinning (`mlock(2)`, `mlockall(MCL_CURRENT | MCL_FUTURE)`) and NUMA-aware allocation (`numa_alloc_onnode()`) to ensure that data structures accessed by pinned threads reside on the DRAM bank attached to the same NUMA node as the pinned core. A cross-NUMA memory access adds ~40–80 ns of latency.

### 3.4 Transparent Huge Pages (THP)
HFT systems typically **disable** THP (`echo never > /sys/kernel/mm/transparent_hugepage/enabled`) to avoid unpredictable page-collapse latency, while **manually** allocating 2 MB hugepages (`MAP_HUGETLB`) for ring buffers and order books to reduce TLB pressure.

---

## 4. Impact on Latency

### 4.1 Elimination of Context-Switch Overhead
Without pinning, the OS scheduler may migrate a thread to a cold core. The costs incurred are:

| Cost Component | Magnitude |
|---|---|
| L1/L2 cache warm-up (cold miss cascade) | 50–500 ns |
| TLB shootdown and refill | 100–300 ns |
| Branch predictor retraining | 5–50 ns |
| Pipeline flush on migration | ~10 ns |

By pinning the strategy thread to a fixed core, the L1i (instruction cache) and L1d (data cache) remain warm across loop iterations. For a tight market-data-processing loop, this can reduce per-message processing time from ~2–5 µs to **200–500 ns**.

### 4.2 Elimination of Scheduler Preemption
An isolated, pinned thread running in a tight polling loop (`SCHED_FIFO` or `SCHED_RR` with maximum priority, or busy-spinning without yielding) is **never preempted** by lower-priority tasks. The only interruptions remaining are:
- NMIs (Non-Maskable Interrupts) — unavoidable.
- SMIs (System Management Interrupts) from firmware — mitigated by BIOS configuration (disable C-states, P-states, Intel SpeedStep, Turbo Boost in some deployments).
- Any interrupt not excluded by `isolcpus`.

### 4.3 Cache Architecture Exploitation
Modern CPUs (e.g., Intel Sapphire Rapids, AMD Genoa) have per-core L1/L2 caches and a shared LLC (L3). CPU pinning enables:

- **False sharing elimination**: With known core placement, data structures can be explicitly padded to cache-line boundaries (64 bytes on x86) so that producer and consumer cores never contend on the same cache line.
- **Prefetch effectiveness**: Hardware prefetchers are trained on the access patterns of a specific core. Thread migration resets this training.
- **Core-local spin-locks**: Spin-locks in the hot path degrade gracefully when both the lock holder and waiter are pinned — the coherence traffic is predictable and bounded.

---

## 5. Impact on Jitter

Jitter in HFT is defined as the **variance (or tail latency)** of the end-to-end processing time distribution, not the mean. A system with 500 ns median latency but 50 µs 99.99th-percentile latency is far worse than one with 800 ns median and 2 µs at the 99.99th percentile.

### 5.1 Sources of Jitter Eliminated by Pinning

| Jitter Source | Mechanism | Eliminated by Pinning? |
|---|---|---|
| Scheduler migration | Thread moved to cold core | ✅ Yes |
| Scheduler tick interrupt | Periodic OS clock tick | ✅ With `nohz_full` |
| NIC IRQ landing on hot core | Interrupt handler preempts thread | ✅ With IRQ affinity |
| RCU callbacks | Async kernel bookkeeping | ✅ With `rcu_nocbs` |
| THP collapse | OS background page merge | ✅ With THP disabled |
| NUMA remote access | Memory on wrong node | ✅ With NUMA pinning |
| SMI (firmware) | BIOS/UEFI background tasks | ⚠️ Partially (BIOS config) |
| NMI | Hardware watchdog, PMU | ❌ Cannot eliminate |
| LLC contention | Sibling core evicting LLC lines | ⚠️ Partially (core isolation) |

### 5.2 Residual Jitter Sources

After full isolation, the dominant residual jitter sources are:

1. **DRAM refresh cycles**: DDR5 refresh events impose ~200–300 ns stalls roughly every 7.8 µs. Mitigation: use ECC-off configurations or exploit `max_act` tuning.
2. **PCIe transaction layer latency variability**: Kernel bypass (DPDK/RDMA) reduces but doesn't eliminate PCIe latency variance (~50–150 ns tail).
3. **Thermal throttling**: Even with P-state disabled, sustained high load can trigger thermal margin throttling. Fixed-frequency operation (`cpupower frequency-set -g performance`) and direct liquid cooling are standard.
4. **Power delivery transients**: Voltage regulator response to sudden load changes (e.g., core waking from a stall) injects ~50–100 ns jitter. Mitigated by keeping the core fully active (busy-polling, never sleeping).

### 5.3 Jitter Measurement and Quantification

HFT shops measure jitter with hardware timestamping:

```
Jitter = P99.99(latency) - P50(latency)
```

Typical observed improvements from full CPU isolation + pinning:

| Configuration | P50 Latency | P99 Latency | P99.99 Latency |
|---|---|---|---|
| Default Linux, no pinning | 2–5 µs | 50–200 µs | 500 µs–5 ms |
| Pinning only | 1–3 µs | 10–30 µs | 100–500 µs |
| Pinning + `isolcpus` + `nohz_full` | 300–800 ns | 2–10 µs | 10–50 µs |
| Full stack (above + kernel bypass + hugepages) | 150–500 ns | 500 ns–2 µs | 2–10 µs |

---

## 6. Hyperthreading (SMT) Considerations

Hyperthreading is **almost always disabled** in HFT cores. The reason is that two sibling hardware threads share:

- **L1 data and instruction caches** (on Intel, fully shared)
- **Execution unit resources** (ALUs, FPUs, load/store units)
- **TLB entries**

A sibling thread running any workload (even a busy-spin NOP loop) degrades the performance of the pinned hot-path thread through **resource partitioning**. Intel's Hyper-Threading Technology divides execution resources roughly 50/50 between sibling threads when both are active. For a strategy engine thread that is execution-unit-bound (e.g., SIMD order book updates), this is unacceptable.

Disabling SMT is done either in BIOS or via:
```bash
echo off > /sys/devices/system/cpu/smt/control
```

---

## 7. Kernel Bypass and the Role of Pinning in DPDK/SOLARFLARE/RDMA Architectures

In kernel-bypass architectures (DPDK, Solarflare OpenOnload, Mellanox RDMA), the NIC ring buffer is mapped directly into user space. The pinned thread **polls** the RX ring in a tight loop without any syscall:

```c
// DPDK polling loop on pinned core
while (running) {
    nb_rx = rte_eth_rx_burst(port, queue_id, pkts, BURST_SIZE);
    if (nb_rx > 0) process_packets(pkts, nb_rx);
    // No yield, no sleep — pure busy-poll
}
```

CPU pinning is **essential** here because:
1. The RX queue is associated with a specific NIC hardware queue, which is mapped to a specific CPU core via MSI-X interrupt routing.
2. The DPDK PMD (Poll Mode Driver) uses per-lcore (logical core) data structures that must not be accessed concurrently or migrated.
3. Any migration would cause the thread to poll a queue whose coherence domain is on a different NUMA node, adding ~40–80 ns per packet.

---

## 8. Practical Topology Selection

Selecting *which* cores to pin to is as important as pinning itself. The criteria:

- **Physical core, not logical**: Avoid HT siblings.
- **Same NUMA node as NIC PCIe attachment**: Verified via `cat /sys/bus/pci/devices/<BDF>/numa_node`.
- **Adjacent cores sharing L2**: On ring-bus and mesh architectures (Intel), L2 is private but L3 slices are distributed. Cores sharing an L3 slice minimize LLC-to-LLC transfer latency (~10 ns vs ~40 ns cross-slice).
- **Avoid core 0**: Core 0 is typically the boot CPU and receives many OS interrupts by default.
- **Verify with `lstopo`** (hwloc): Visualize cache topology before committing to a pinning scheme.

---

## Summary

CPU pinning in HFT is not merely a performance tuning knob — it is an architectural primitive that transforms a general-purpose multi-tasking OS into a near-deterministic real-time execution environment. Its effectiveness is multiplicative with `isolcpus`, `nohz_full`, IRQ affinity, NUMA binding, hugepages, and kernel bypass. The primary contributions are the **elimination of cache-cold migration overhead** (cutting mean latency by 4–10×) and the **suppression of scheduler-induced tail latency** (reducing P99.99 jitter by 2–3 orders of magnitude). What remains — DRAM refresh, SMI, NMI, and PCIe variance — represents the irreducible noise floor of commodity x86 hardware, and pushing below it requires custom ASICs or FPGAs.


---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Describe the CPU pinning in the high-frequency trading system in depth. This description is intended for a computer science expert. Analyze the impact of CPU pinning on latency and jitter.
