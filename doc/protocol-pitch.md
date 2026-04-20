# CBOE/BATS PITCH Protocol — Technical Deep Dive

## Overview

PITCH (Price/Quote/Trade) is a binary market data protocol developed by BATS Global Markets (now part of CBOE Global Markets). It is the primary market data feed format for CBOE's U.S. equities and options exchanges: BZX, BYX, EDGX, EDGA, and C2 Options. It delivers a **full order-book feed** — every order add, cancel, modify, and execution — enabling subscribers to reconstruct the complete Level 2 order book in real time. In HFT, PITCH is one of the most latency-sensitive feeds consumed, often processed within single-digit microseconds per message.

---

## Transport Layer

### UDP Multicast (Primary)
PITCH is distributed via **UDP multicast**, not TCP. This is a deliberate, fundamental design choice:

- **Zero per-subscriber overhead**: The exchange transmits one stream; thousands of subscribers receive it simultaneously with no server-side fan-out cost.
- **No TCP head-of-line blocking**: A lost packet does not stall the stream; the receiver processes what it has and gaps are handled at the application layer.
- **Predictable, minimal latency**: UDP imposes no ACK round-trips, no congestion window, no Nagle algorithm.

Subscribers join a multicast group (IGMP) on a dedicated network interface. In co-location environments (e.g., CBOE's Secaucus NJ facility), feeds are delivered over 10GbE or 25GbE with PTP/IEEE 1588-synchronized timestamps.

### Gap Fill via TCP (Retransmission)
Because UDP drops are possible, PITCH provides a **GapFill / spin server** over TCP:

- Upon detecting a sequence gap (see below), a subscriber connects to the retransmission server and requests the missing sequence range.
- The TCP retransmit path is high-latency relative to the live feed; in practice, HFT firms either tolerate short gaps or run redundant multicast feeds (Unit A + Unit B) from different network paths and cross-stitch them.

---

## Message Framing and Wire Format

### Sequenced Unit Header
Every UDP datagram begins with a fixed **Sequenced Unit Header**:

```
+--------+--------+--------+--------+--------+--------+--------+--------+
| Length | Count  |  Unit  |     Sequence Number (uint32_t, LE)         |
| uint16 | uint8  |  uint8 |                                            |
+--------+--------+--------+--------+--------+--------+--------+--------+
  0-1      2        3        4-7
```

- **Length** (2 bytes, LE): Total datagram length including this header.
- **Count** (1 byte): Number of PITCH messages packed into this datagram (batching).
- **Unit** (1 byte): Logical feed unit (1–8 per exchange), partitioning symbols across units for parallelism.
- **Sequence Number** (4 bytes, LE): Monotonically increasing per unit. The first message in the datagram has this sequence number; subsequent messages have implicit sequence numbers `seq, seq+1, ...seq+count-1`.

The sequence number is the **heartbeat and gap detector** rolled into one. If you receive seq=1001 after seq=999, you know message 1000 is missing.

### Individual Message Format

Each PITCH message is prefixed by:

```
+--------+--------+
| Length | MsgTyp |
| uint8  |  char  |
+--------+--------+
  0        1
```

No padding or alignment is guaranteed beyond this. All multi-byte integers are **little-endian**. All prices are encoded as **fixed-point integers** (typically in units of 1/10,000th of a dollar, i.e., a `uint64_t` where `100000000` = $10,000.0000). This avoids floating-point entirely — critical for deterministic, branch-free processing.

---

## Core Message Types

### Order Management Messages

| Type | Char | Description |
|---|---|---|
| Add Order (short) | `A` | New order, 6-char symbol, no participant ID |
| Add Order (long) | `d` | New order, 8-char symbol |
| Add Order – Attributed | `A`/`D` | Includes MPID (market participant ID) |
| Order Executed | `E` | Full or partial fill, references Order ID |
| Order Executed w/ Price | `C` | Fill at a price different from order price (e.g., price improvement) |
| Reduce Size | `X` | Partial cancel — reduce qty remaining |
| Modify Order | `e` | Change price and/or qty (non-cancel-replace) |
| Delete Order | `D` | Full cancel |

Every order carries a **12-byte Order ID** (a `uint64_t` in most implementations, though the protocol defines it as a base-36 alphanumeric). This is the primary key for your order book hash map.

### Trade Messages

| Type | Char | Description |
|---|---|---|
| Trade (short/long) | `P` / `r` | Non-displayed/odd-lot trade, does **not** reference a resting order |
| Trade Break | `B` | Bust of a previously reported trade |
| Trading Status | `H` | Halt/resume, auction states |

`Trade` messages (type `P`) represent executions where one or both sides may be non-displayed (e.g., dark pool internalization). They do **not** appear as `Order Executed` messages. An HFT book builder must process both to get accurate trade tape and volume.

### Auction Messages

| Type | Char | Description |
|---|---|---|
| Auction Update | `I` | Real-time auction collar/indicative price |
| Auction Summary | `J` | Final auction price and paired shares |

These are crucial for opening/closing auction strategies. The `Auction Update` message streams continuously as the indicative match price changes.

### Administrative / Control

| Type | Char | Description |
|---|---|---|
| Symbol Clear | `s` | Wipe all orders for symbol (e.g., start of day) |
| End of Session | `Z` | Feed is done for the day |
| Timestamp | `T` | Seconds-precision time anchor (rare, as most messages carry ns timestamps) |

---

## Timestamps

Each order-bearing message includes a **nanosecond timestamp** encoded as a `uint32_t` offset in nanoseconds from the most recent `Timestamp` (`T`) message (which carries whole seconds). The full timestamp is:

```cpp
uint64_t full_ns = (uint64_t)last_timestamp_seconds * 1_000_000_000ULL + msg.ns_offset;
```

This timestamp is the **exchange-assigned send time** (from the matching engine's clock), not the NIC receive time. In an HFT context, you compare this against your own `CLOCK_REALTIME`/`rdtsc`-derived receive timestamp to measure **wire latency** and detect feed slowdowns.

---

## Book Construction in C++

### Data Structures

The canonical HFT book structure for PITCH:

```cpp
// Price level aggregation
struct PriceLevel {
    uint64_t price;      // Fixed-point, 1/10000 dollar
    uint64_t qty;
    uint32_t order_count;
};

// Per-order tracking (needed for Reduce/Delete/Execute)
struct Order {
    uint64_t order_id;
    uint64_t price;
    uint32_t qty;
    char     side;       // 'B' or 'S'
    uint32_t symbol_idx; // index into symbol table
};

// The book per symbol
struct OrderBook {
    // Bids: descending price → use std::map<uint64_t, PriceLevel, std::greater<>>
    // Asks: ascending  price → use std::map<uint64_t, PriceLevel>
    // In HFT, replace with a flat sorted array or a skip list for cache locality.
    PriceLevel bids[MAX_LEVELS];
    PriceLevel asks[MAX_LEVELS];
    int        bid_count, ask_count;
};

// Global order map — the hot path lookup
std::unordered_map<uint64_t, Order> order_map; // or a robin-hood/flat hash map
```

**Avoid `std::map` in production**: its pointer-chasing is cache-hostile. Use a cache-oblivious sorted array for the top N levels (N ≤ 10 covers 99%+ of trading decisions) and a fallback for deep book.

### Message Dispatch — Zero-Copy, Branch-Free

```cpp
void process_datagram(const uint8_t* buf, size_t len) {
    const PitchHeader* hdr = reinterpret_cast<const PitchHeader*>(buf);
    uint32_t seq = le32toh(hdr->sequence);
    
    if (seq != expected_seq_[hdr->unit]) {
        handle_gap(hdr->unit, expected_seq_[hdr->unit], seq);
    }
    expected_seq_[hdr->unit] = seq + hdr->count;
    
    const uint8_t* ptr = buf + sizeof(PitchHeader);
    const uint8_t* end = buf + le16toh(hdr->length);
    
    while (ptr < end) {
        uint8_t msg_len = ptr[0];
        char    msg_type = (char)ptr[1];
        dispatch(msg_type, ptr + 2, msg_len - 2);
        ptr += msg_len;
    }
}
```

The `dispatch` function is typically a **jump table** (array of function pointers indexed by `msg_type`), not a switch statement. The compiler may generate a switch as a jump table anyway, but explicit control guarantees it and enables inlining hints:

```cpp
using Handler = void(*)(const uint8_t*, uint8_t);
static Handler jump_table[256] = {};  // initialized at startup

// In init:
jump_table[(uint8_t)'A'] = handle_add_order_short;
jump_table[(uint8_t)'d'] = handle_add_order_long;
jump_table[(uint8_t)'E'] = handle_order_executed;
// ...
```

---

## Gap Handling and Sequencing

Gap handling is operationally critical. A missed sequence number means your book is **corrupt** — you may have phantom liquidity or missing orders, leading to adverse fills or missed opportunities.

### Strategies (in order of preference in HFT)

1. **Dual-feed cross-stitching** (A/B feeds): CBOE publishes each unit on two separate multicast groups (A and B) via different network paths. Run two receiver threads, each writing to a lock-free ring buffer per unit. A merger thread picks the lowest sequence number seen from either, deduplicated by sequence number. This achieves near-zero gap rate.

2. **Gap toleration with book reset**: For units where a gap exceeds a threshold (e.g., >100ms), request a snapshot from the CBOE PITCH Spin Server (a TCP-based "top of book" or "full depth" snapshot) and resync. The spin server delivers a `Symbol Clear` equivalent followed by all current resting orders.

3. **TCP retransmit**: Request specific sequence ranges. Usable for small gaps but adds 100µs–10ms latency (TCP handshake + processing). Not suitable for live trading decisions; useful for post-trade reconciliation.

---

## Feed Units and Parallelism

Each exchange (e.g., BZX) partitions its symbol universe across **up to 8 units**, each on its own multicast group. The partitioning is deterministic per symbol (based on a hash of the ticker). This allows:

- **Parallel processing**: One CPU core (pinned, isolated) per unit, no cross-unit synchronization for book updates.
- **Selective subscription**: A firm trading only S&P 500 names can subscribe to only the units that carry those symbols, reducing CPU load and NIC buffer pressure.

In C++, this naturally maps to a thread-per-unit architecture:

```
Unit 1 → CPU core 4 → ring buffer → strategy thread A
Unit 2 → CPU core 5 → ring buffer → strategy thread B
...
```

Use **CPU affinity** (`pthread_setaffinity_np`) and **NUMA awareness** to keep the NIC, ring buffer, and processing core on the same NUMA node.

---

## Kernel Bypass and NIC Integration

In co-location, the UDP stack is bypassed entirely:

- **DPDK** or **Solarflare OpenOnload / ef_vi**: The NIC DMA's packets directly into application-managed hugepage memory. The application polls the NIC descriptor ring (`busy poll`) rather than waiting for a kernel interrupt. This reduces NIC-to-application latency from ~5–15µs to ~500ns–2µs.
- **FPGA offload**: Some firms push PITCH parsing entirely onto an FPGA (e.g., Xilinx Alveo, Solarflare SFN8000). The FPGA parses the binary protocol, updates a hardware order book, and signals the CPU only on configurable events (e.g., BBO change, large order arrival). End-to-end latency from packet arrival to trading signal: < 1µs.

The fixed-size, deterministic binary format of PITCH is purpose-built for this kind of hardware parsing — there is no variable-length string scanning, no delimiter search, no dynamic dispatch based on runtime-determined offsets.

---

## Business / Market Microstructure Features

### Full Depth of Book
Unlike SIP feeds (CTA/UTP), PITCH provides **every resting order** at every price level, not just the NBBO. This enables:
- Hidden liquidity detection (compare PITCH order qty vs. trade qty to infer hidden orders)
- Adverse selection modeling
- Queue position estimation (FIFO priority queues at each price level)

### Participant Attribution (MPID)
The attributed `Add Order` message includes the **Market Participant ID** — a 4-character identifier for the firm posting the order. This is published for displayed, non-anonymous orders. HFT strategies use MPID data to:
- Detect market maker withdrawal (flight-to-quality signal)
- Identify toxic flow sources
- Model competitor behavior

### Retail Designation
CBOE BZX/BYX carry a **Retail Price Improvement** flag on certain order types. PITCH encodes this in the order attributes field, allowing firms to distinguish retail flow (statistically less informed, safer to internalize or provide against).

### Order Attributes Byte
The `Add Order` message includes a bitmask of order attributes:
- `D` bit: **Display** (visible in PITCH) vs. non-displayed
- `I` bit: **Intermarket Sweep Order** (ISO) — will trade through NBBO, used for aggressive cross-market sweeps
- `P` bit: **Post-Only** — will not execute if it would take liquidity

These attributes are essential for inferring order intent and modeling fill probability.

### Trading Halts and Auctions
The `Trading Status` message encodes the **halt reason** (LULD band breach, regulatory halt, news pending) and **auction state** (opening, closing, IPO, halt auction). A robust HFT system must track this state machine per symbol and suppress or modify behavior accordingly — sending aggressive orders into a halted market wastes capacity and may trigger exchange-side rejections.

---

## Latency Budget (Representative, Co-location)

| Stage | Latency |
|---|---|
| Photons off fiber → NIC silicon (kernel bypass) | ~100–300 ns |
| NIC DMA + descriptor ring poll | ~200–500 ns |
| PITCH header parse + sequence check | ~50–100 ns |
| Message dispatch + book update | ~100–300 ns |
| BBO change detection + signal generation | ~50–150 ns |
| **Total: wire → trading signal** | **~500 ns – 1.5 µs** |

An FPGA implementation compresses this to 100–400 ns total.

---

## Common Implementation Pitfalls

- **Endianness**: All PITCH integers are **little-endian**. On x86 this is a no-op, but always use `le32toh`/`le64toh` for portability and documentation clarity.
- **Order ID collisions**: Order IDs are unique per unit, **not** globally. Use `{unit, order_id}` as your hash map key if you merge units.
- **Symbol Clear** (`s`): If ignored, you will carry stale orders across days or after a feed reconnect. Always implement this.
- **Trade Break** (`B`): Trade bust messages arrive out-of-band relative to volume accumulators. If you tally daily volume from PITCH, you must subtract busted trades.
- **Sequence wrap**: The `uint32_t` sequence number wraps at ~4 billion. At peak CBOE volume (~5M messages/day/unit) this is a ~2-year wrap, but your code should handle it.
- **Batching**: Multiple messages per datagram is the norm at high volume. Never assume `count == 1`.

---

## Summary

PITCH is a masterclass in protocol design for extreme-throughput, low-latency environments: binary, little-endian, fixed-point prices, datagram-batched, multicast-distributed, with a minimal header that doubles as a gap detector. For a C++ HFT engineer, consuming it well means lock-free ring buffers, kernel bypass networking, jump-table dispatch, cache-aware data structures, and a disciplined state machine for every message type. The protocol's structure maps almost perfectly to what modern CPUs and FPGAs process most efficiently — and that is entirely intentional.

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Provide a thorough description of the PITCH (CBOE/BATS Market Data Protocol) protocol in a high-frequency trading environment. This description is intended for a computer science expert fluent in C++. Describe the important technical and business features of the protocol.
