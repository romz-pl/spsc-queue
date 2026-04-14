# x86-64 CPUs for High-Frequency Trading: A Hardware Reference

HFT workloads are pathologically sensitive to memory latency and determinism. A cache miss to DRAM can cost 60–100 ns — enough to lose a trade entirely. The processors below are the ones that appear most frequently in co-location racks, either as primary execution engines or as the latency-optimised cores to which order-entry threads are pinned.

---

## 1. Intel Xeon Scalable 4th Generation — "Sapphire Rapids" (SPR, 2023)

**Representative SKUs:** Xeon Platinum 8490H, 8480+, 8462Y+; Xeon Gold 6448Y, 6438Y

### Architecture overview
Sapphire Rapids is the first Intel server die to adopt a **tile-based chiplet design**, stitching four compute tiles together with EMIB (Embedded Multi-die Interconnect Bridge). Each tile contains a subset of cores sharing a slice of the distributed L3. This topology has profound implications for HFT: cross-tile L3 access adds an extra hop and should be avoided by pinning hot threads to cores within a single tile.

### Cache hierarchy

| Level | Size per core | Associativity | Access latency |
|---|---|---|---|
| L1-I | 32 KB | 8-way | ~4 cycles |
| L1-D | 48 KB | 12-way | ~4 cycles |
| L2 | 2 MB | 16-way | ~14 cycles |
| L3 | ~1.875 MB/core (distributed), up to 112.5 MB (Platinum 8490H) | 12-way | ~40–55 cycles (local tile) / ~70+ cycles (remote tile) |

The **2 MB L2** is the most significant SPR cache change relative to Ice Lake's 1.25 MB L2. For HFT applications, this keeps substantially larger hot code paths and order-book working sets out of the high-latency L3. The L1-D was also expanded from 32 KB to 48 KB (12-way), directly benefiting algorithms with tight inner loops over small arrays.

### Clock speeds
- Base: 1.9–2.9 GHz (depending on SKU and thermal envelope)
- All-core turbo: up to 3.5 GHz (Gold 6448Y)
- Single-core turbo: up to 4.0 GHz (Platinum 8490H)

For latency-sensitive threads, disable Turbo Boost and fix the P-state via `cpupower frequency-set` or MSR writes to eliminate clock-speed jitter.

### HFT-specific notes
- **AMX (Advanced Matrix Extensions)** is irrelevant for tick-to-trade paths but useful for ML-based signal generation co-located on the same host.
- **DDR5-4800** and **HBM2e** (on the 8490H) provide massive bandwidth for market-data fan-out.
- The four-tile topology means NUMA within a socket: bind order-entry threads to a single tile using `taskset` with tile-aware core masks.
- PCIe 5.0 is critical for next-generation SmartNICs (Nvidia BlueField-3, Solarflare X2541) and FPGA boards (Xilinx Alveo U55C) now shipping with Gen5 interfaces.

---

## 2. Intel Xeon Scalable 3rd Generation — "Ice Lake-SP" (ICX, 2021)

**Representative SKUs:** Xeon Platinum 8380, 8368; Xeon Gold 6338, 6354

### Architecture overview
A mature, well-understood monolithic die. No cross-tile penalty. The uniform L3 ring-bus topology makes latency more predictable than SPR — still favoured in some production systems precisely because the cache fabric is simpler to reason about.

### Cache hierarchy

| Level | Size per core | Associativity | Access latency |
|---|---|---|---|
| L1-I | 32 KB | 8-way | ~4 cycles |
| L1-D | 32 KB | 8-way | ~4 cycles |
| L2 | 1.25 MB | 20-way | ~14 cycles |
| L3 | ~1.5 MB/core (distributed ring), up to 60 MB | 12-way | ~38–50 cycles |

The **1.25 MB L2** is a notable regression from SPR for working-set-sensitive order books. However, the **monolithic ring bus** eliminates the tile-boundary latency spike seen in SPR, making worst-case L3 latency more uniform — a property that matters as much as average latency in HFT.

### Clock speeds
- Base: 2.1–2.9 GHz
- Single-core turbo: up to 3.9 GHz (Platinum 8380)

### HFT-specific notes
- Ice Lake-SP was the first Intel server generation with **AVX-512** at full throughput (two AVX-512 FMA units per core), useful for vectorised order-matching and risk calculation.
- Well-supported by kernel tuning guides (Red Hat, Ubuntu) with stable, predictable latency under `isolcpus` + `nohz_full` + `rcu_nocbs` isolation.
- Still widely deployed in co-lo due to lower acquisition cost and proven production track record.

---

## 3. AMD EPYC 9004 — "Genoa" (Zen 4, 2022–2023)

**Representative SKUs:** EPYC 9654, 9554, 9354P, 9224

### Architecture overview
Genoa uses up to **12 CCD (Core Complex Die) chiplets** connected to a central I/O die (IOD) via AMD's Infinity Fabric. Each CCD contains 8 Zen 4 cores. The L3 is partitioned per CCD: each CCD has its own **32 MB L3 slice**, giving up to **384 MB of total L3** on a 96-core part — the largest cache pool available on any x86-64 server as of 2024.

### Cache hierarchy

| Level | Size per core | Associativity | Access latency |
|---|---|---|---|
| L1-I | 32 KB | 8-way | ~4 cycles |
| L1-D | 32 KB | 8-way | ~4 cycles |
| L2 | 1 MB | 8-way | ~12 cycles |
| L3 (local CCD) | 32 MB per CCD (4 MB/core) | 16-way | ~40–47 cycles |
| L3 (remote CCD) | — | — | ~100–130 cycles (Infinity Fabric hop) |

The **32 MB local L3** per CCD is the defining HFT feature of Genoa. Pinning an order-entry thread and its supporting threads to a single CCD (using `taskset` or `numactl` with CCD-aware core masks) can keep an enormous order book — potentially including full depth-of-book for multiple symbols — entirely within L3 without touching DRAM.

The **1 MB L2** per Zen 4 core (double Zen 3's 512 KB) further reduces L3 pressure for hot loops.

### Infinity Fabric latency
Cross-CCD traffic travels via the IOD, adding ~60–90 ns. This is the cardinal sin to avoid in HFT thread placement. AMD's `amd-topology` tooling and BIOS-exposed CCD affinity masks should be used to derive exact pinning.

### Clock speeds
- Base: 2.25–3.7 GHz
- Single-core boost: up to 3.75 GHz (9654), 3.85 GHz (9554)

### HFT-specific notes
- EPYC 9004's **Infinity Fabric frequency** (nominally 1800 MHz for DDR5-3600) directly affects cross-CCD latency. Some operators underclock memory to reduce Fabric jitter.
- **CXL 1.1** support opens the door to CXL-attached low-latency memory expanders, though production adoption is nascent.
- The high core count allows co-hosting of market-data handlers, strategy engines, and risk engines on isolated CCDs with near-zero cross-contamination — a compelling operational architecture.

---

## 4. AMD EPYC 7003 — "Milan" (Zen 3, 2021)

**Representative SKUs:** EPYC 7763, 7713, 7543, 7453

### Architecture overview
Milan introduced a unified L3 per CCD: all 8 cores in a CCD share a single **32 MB L3 slice** (versus Zen 2's split CCX topology with 16 MB per 4-core CCX). This dramatically reduced worst-case intra-CCD L3 latency and is why Milan is widely regarded as the generation where AMD became fully viable for HFT.

### Cache hierarchy

| Level | Size per core | Associativity | Access latency |
|---|---|---|---|
| L1-I | 32 KB | 8-way | ~4 cycles |
| L1-D | 32 KB | 8-way | ~4 cycles |
| L2 | 512 KB | 8-way | ~12 cycles |
| L3 (local CCD) | 32 MB per CCD (4 MB/core) | 16-way | ~40–47 cycles |
| L3 (remote CCD) | — | — | ~105–140 cycles |

The **512 KB L2** is the primary weakness versus both Genoa and SPR. Hot code paths exceeding 512 KB (e.g., large templated C++ market-data handlers compiled without LTO) will spill into L3 more aggressively. Profile-guided optimisation and careful object layout are particularly important on Milan.

### Clock speeds
- Base: 2.0–3.5 GHz
- Single-core boost: up to 3.6 GHz (7763), 3.7 GHz (7543)

### HFT-specific notes
- Milan remains the **most widely deployed EPYC generation in HFT production** as of 2024, owing to its maturity, kernel/driver stability, and known latency profile.
- The unified 32 MB L3 per CCD is architecturally identical to Genoa, making code tuned for Milan CCD-pinning directly portable.

---

## 5. Intel Core i9 / i7 — 13th & 14th Gen ("Raptor Lake") — Ultra-Low-Latency Specialist

**Representative SKUs:** Core i9-14900K, i9-13900K, i7-13700K

### Architecture overview
Consumer-grade but increasingly found in **proximity hosting** and **single-strategy backtesting / paper-trading** infrastructure, and occasionally in co-lo for strategies where the highest possible single-core clock matters more than core count or ECC. The P-cores (Performance cores) use a Golden Cove / Raptor Cove microarchitecture very similar to SPR server cores.

### Cache hierarchy (P-core only)

| Level | Size | Associativity | Access latency |
|---|---|---|---|
| L1-I | 32 KB per P-core | 8-way | ~4 cycles |
| L1-D | 48 KB per P-core | 12-way | ~4 cycles |
| L2 | 2 MB per P-core | 16-way | ~14 cycles |
| L3 (shared) | 36 MB (i9-14900K) | 12-way | ~40–50 cycles |

The **48 KB L1-D and 2 MB L2 per P-core** are identical to SPR, giving excellent hot-path performance. The E-cores (Efficient cores) should be **disabled in BIOS** for HFT use — they share L3 but have smaller private caches and introduce scheduling uncertainty.

### Clock speeds
- P-core base: 3.2 GHz (i9-14900K)
- P-core boost: up to **6.0 GHz** (i9-14900K) — highest x86-64 clock available on any production silicon as of 2024
- Recommended HFT operating point: fix P-state at 5.2–5.6 GHz with a quality AIO cooler to eliminate boost variance

### HFT-specific notes
- **No ECC support** — a hard blocker for most regulated HFT environments. Used only where ECC is not mandated or where a separate risk system handles correction.
- Single-socket only, no NUMA complexity.
- Ideal for **FPGA-offloaded** architectures where the CPU handles only final decision logic and order submission; the simplicity of a single-socket consumer platform with a locked high clock can win over a dual-socket server with complex NUMA topology.

---

## Cross-Architecture Cache Latency Summary

| Processor | L1-D | L2 | L3 (local) | L3 (remote tile/CCD) | DRAM |
|---|---|---|---|---|---|
| SPR Xeon Platinum | ~1.0 ns | ~4.5 ns | ~14–18 ns | ~25–35 ns | ~70–90 ns |
| ICX Xeon Platinum | ~1.2 ns | ~4.5 ns | ~13–16 ns | N/A (monolithic) | ~70–85 ns |
| EPYC Genoa (Zen 4) | ~1.2 ns | ~4.0 ns | ~14–17 ns | ~35–50 ns | ~75–95 ns |
| EPYC Milan (Zen 3) | ~1.2 ns | ~4.0 ns | ~14–17 ns | ~38–55 ns | ~75–95 ns |
| Core i9-14900K | ~1.0 ns | ~4.0 ns | ~12–15 ns | N/A (monolithic) | ~55–70 ns |

*All figures assume fixed P-state, C-states disabled, cache isolation in effect. Measured with a 5 GHz fixed clock and typical DDR5-4800.*

---

## Production Tuning Essentials (applicable to all above)

These BIOS/OS settings are non-negotiable for latency-critical deployments on any of the above platforms:

- **Disable all C-states** (`idle=poll` kernel parameter or BIOS C-state limit = C0). C1 exit latency alone can add 1–10 µs of jitter.
- **Disable Turbo Boost / Precision Boost Overdrive** and fix the P-state to eliminate frequency ramp latency.
- **Disable Hyper-Threading / SMT** on cores running latency-critical threads to eliminate shared L1/L2 interference from sibling threads.
- **Isolate CPUs** with `isolcpus=`, `nohz_full=`, and `rcu_nocbs=` to remove scheduler, timer-tick, and RCU jitter from hot cores.
- **NUMA-aware allocation**: use `numactl --membind` to ensure all memory allocations for a thread are local to its socket/tile/CCD. Remote NUMA accesses on a dual-socket SPR or multi-CCD EPYC platform can exceed 150 ns.
- **Disable Spectre/Meltdown mitigations** (`mitigations=off`) on isolated HFT hosts — the `retpoline` and IBPB mitigations add measurable latency to indirect call-heavy market-data parsing code.
- **Transparent Huge Pages**: set to `always` for large order-book data structures to reduce TLB misses. The L1 dTLB on all the above architectures is 64 entries (4 KB pages) or 8 entries (2 MB pages); a 2 MB TLB entry covers 512× more data per entry.
- **IRQ affinity**: pin all NIC interrupts away from isolated cores using `/proc/irq/*/smp_affinity`.


---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Provide an in-depth description of the most popular x86-64 CPUs used in high-frequency trading systems. This description is intended for computer science experts who wish to use the hardware in production HFT systems. Emphasise the speed and the size of the L1, L2 and L3 caches, as these are crucial for latency and jitter. 
