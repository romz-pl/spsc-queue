# NASDAQ TotalView-ITCH 5.0: A Deep Technical Reference for HFT

---

## 1. Protocol Overview & Design Philosophy

ITCH 5.0 is a **unidirectional, UDP-multicast market data feed** published by NASDAQ. It is the raw order book feed — not a consolidated tape — meaning it delivers the full lifecycle of every order on the NASDAQ exchange: adds, cancels, executions, and replacements. It is the canonical input to any serious HFT order book reconstruction engine.

The protocol's defining design choices all serve a single master: **throughput at the cost of flexibility**. There is no TCP handshake, no ACK, no retransmission at the network layer. You either receive the packet or you don't. This makes it ideal for a collocated HFT firm where packet loss is near-zero and microseconds matter, but it demands that your application handle gaps and sequencing entirely on its own.

---

## 2. Transport Layer

### Multicast over UDP (MoldUDP64)
ITCH messages are not sent raw. They are wrapped in **MoldUDP64**, a thin framing protocol designed by NASDAQ. MoldUDP64 adds:

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Session (10 bytes ASCII)                  |
+                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                               |    Sequence Number (8 bytes)  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Message Count (2 bytes)     | [ITCH Messages...]            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               |
```

- **Session**: A 10-byte ASCII identifier for the trading day. New session = new sequence space.
- **Sequence Number**: A monotonically increasing 64-bit integer, per session, per multicast channel. This is your gap detector.
- **Message Count**: Number of ITCH messages bundled in this datagram. Multiple messages are batched per UDP packet to amortize kernel overhead.

### Gap Recovery via TCP (MoldUDP64 Request Server)
When your sequence number jumps — say you receive seq 1001 after seq 998 — you have a gap. The recovery path is a **TCP-based retransmission request** to NASDAQ's retransmission server (also MoldUDP64-framed). In practice, in a collocated environment, gaps are extremely rare and the retransmit path is a fallback, not a normal-path concern. The critical insight is: **never let gap recovery stall your primary processing pipeline**. These must run on separate threads.

---

## 3. Message Framing

Each ITCH message inside a MoldUDP64 packet is length-prefixed:

```
+------------------+------------------+-------------------+
|  Length (2 bytes)|  Type (1 byte)   |  Payload (N bytes)|
+------------------+------------------+-------------------+
```

All integer fields are **big-endian** (network byte order). A performant C++ decoder will `bswap` on read, ideally with `__builtin_bswap16/32/64` or a compile-time-dispatched `std::byteswap` (C++23). All fields are **fixed-width** — there is no variable-length encoding, no TLV, no schema evolution. This allows offset-based field access with zero parsing overhead.

A typical implementation uses a packed struct cast directly onto the buffer:

```cpp
#pragma pack(push, 1)
struct AddOrderMessage {
    uint16_t length;
    char     msg_type;         // 'A'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];     // 48-bit nanoseconds since midnight
    uint64_t order_ref_num;
    char     buy_sell;         // 'B' or 'S'
    uint32_t shares;
    char     stock[8];         // Right-padded with spaces
    uint32_t price;            // Fixed-point: divide by 10,000
};
#pragma pack(pop)
```

The direct-cast approach avoids any copy and is safe on x86-64 (which tolerates unaligned reads), though a pedantically correct implementation would use `memcpy` into the struct to avoid UB.

---

## 4. Timestamp Format

The timestamp is a **48-bit unsigned integer** representing nanoseconds since midnight Eastern Time. It is stored in 6 bytes, big-endian. Deserializing it:

```cpp
inline uint64_t parse_timestamp(const uint8_t* ts) {
    return (uint64_t(ts[0]) << 40) | (uint64_t(ts[1]) << 32) |
           (uint64_t(ts[2]) << 24) | (uint64_t(ts[3]) << 16) |
           (uint64_t(ts[4]) <<  8) |  uint64_t(ts[5]);
}
```

At 1 ns resolution this gives ~97 seconds of range per 48-bit field, comfortably covering a full trading day. This timestamp reflects **NASDAQ's internal clock**, not your NIC's hardware timestamp. For true latency measurement, you'll correlate the ITCH timestamp with a PTP-synchronized hardware timestamp from your kernel bypass NIC (e.g., a Solarflare or Mellanox card using `SO_TIMESTAMPING` or Onload).

---

## 5. Message Taxonomy

### 5.1 System & Administrative

| Type | Name | Description |
|---|---|---|
| `S` | System Event | Market open/close signals (`O`, `C`, `Q`, etc.) |
| `R` | Stock Directory | Instrument reference data at session start |
| `H` | Stock Trading Action | Halts, resumes, quotation-only periods |
| `Y` | Reg SHO Restriction | Short-sale circuit breaker status |
| `L` | Market Participant Position | MM/specialist quoting obligations |
| `V` | MWCB Decline Level | Market-wide circuit breaker thresholds |
| `W` | MWCB Status | MWCB breach status |
| `K` | IPO Quoting Period Update | Pre-IPO state |
| `J` | LULD Auction Collar | Limit-Up/Limit-Down collar prices |

The `R` (Stock Directory) messages at the start of each session are critical — they populate your **stock locate table**, a 2-byte integer that acts as a fast handle for all subsequent messages, avoiding repeated string comparisons on ticker symbols.

### 5.2 Order Book Messages

These are the messages that drive book reconstruction:

| Type | Name | Key Fields |
|---|---|---|
| `A` | Add Order | `order_ref`, `side`, `shares`, `price` |
| `F` | Add Order (MPID) | Same as `A` + market participant ID |
| `E` | Order Executed | `order_ref`, `executed_shares`, `match_number` |
| `C` | Order Executed w/ Price | Execution at a price different from the limit price |
| `X` | Order Cancel | `order_ref`, `cancelled_shares` (partial cancel) |
| `D` | Order Delete | `order_ref` (full removal) |
| `U` | Order Replace | `orig_ref`, `new_ref`, `shares`, `price` |

### 5.3 Trade & Auction Messages

| Type | Name | Description |
|---|---|---|
| `P` | Non-Cross Trade | Off-book (dark pool) print — does NOT modify the lit book |
| `Q` | Cross Trade | Opening/closing/halt cross execution |
| `B` | Broken Trade | Trade bust — reverse a prior execution |
| `I` | NOII | Net Order Imbalance Indicator, emitted during cross periods |

The `I` (NOII) message is extremely valuable for predicting the opening/closing print price. It contains paired interest quantities, imbalance direction, and a near/far price collar — essential inputs to a cross arbitrage or MOC (Market-On-Close) strategy.

---

## 6. Order Book Reconstruction

### Data Structures

The performance-critical path in any ITCH consumer is the order book. The dual requirements are:

1. **O(1) order lookup** by `order_ref_num` (for `E`, `X`, `D`, `U` messages)
2. **O(log N) or O(1) price level access** for best bid/offer and depth queries

A canonical HFT implementation:

```cpp
struct Order {
    uint64_t ref;
    uint32_t price;   // raw fixed-point
    uint32_t shares;
    uint16_t locate;
    char     side;    // 'B' or 'S'
};

// Order map: ref → Order (for mutation messages)
// Use a flat hash map, NOT std::unordered_map
// Robin Hood hashing or a custom open-addressing table
// avoids pointer chasing and is cache-friendly
absl::flat_hash_map<uint64_t, Order> order_map;

// Price levels per instrument per side
// Array-of-structs at each price point
struct PriceLevel {
    uint32_t price;
    uint32_t total_shares;
    uint32_t order_count;
};

// The book itself: sorted by price
// For bids: descending; for asks: ascending
// A flat sorted array (binary-searched) often beats std::map
// for realistic book depths (< 1000 levels) due to cache effects
```

For the price-level structure, `std::map<uint32_t, PriceLevel>` is the naïve choice but is cache-hostile. A **sorted `std::vector` with binary search** is often faster in practice for typical book depths. For ultra-low-latency, a **price-indexed array** (using price as a direct array index into a pre-allocated slab) gives O(1) level access at the cost of memory:

```cpp
// Price is in units of $0.0001; range $0–$999.9999
// 10,000,000 possible price points × 8 bytes = 80 MB per side per instrument
// Feasible only for focused single-instrument books
```

### Handling Order Replace (`U`)

`U` is a **cancel + add** in one message, with a new `order_ref`. Critically, the new ref is a different key in your hash map. A naive implementation will:
1. Look up `orig_ref` → copy price/shares
2. Erase `orig_ref` from the order map
3. Decrement price level by old shares
4. Insert `new_ref` into the order map
5. Increment price level by new shares

**Price changes in a `U` message lose queue priority** — the replaced order goes to the back of the queue at the new price. This matters for order queue position modeling in maker-rebate strategies.

---

## 7. Price Encoding

All prices in ITCH 5.0 are **32-bit unsigned integers in units of $0.0001** (i.e., 4 decimal places). No floating point is used anywhere in the protocol. In C++:

```cpp
// Never use double for ITCH prices in production
// Fixed-point arithmetic only
constexpr uint32_t PRICE_DENOMINATOR = 10'000;

struct Price {
    uint32_t raw;
    
    // Dollars
    uint32_t dollars() const { return raw / PRICE_DENOMINATOR; }
    // Fractional cents (0-9999)
    uint32_t sub_dollar() const { return raw % PRICE_DENOMINATOR; }
    
    // Format: $XX.XXXX
    std::string to_string() const {
        return std::to_string(dollars()) + "." + 
               std::format("{:04d}", sub_dollar());
    }
};
```

This design is intentional: floating-point rounding errors in a matching engine or P&L calculation are commercially unacceptable.

---

## 8. Stock Locate Table

The `stock_locate` field (2 bytes) is a session-scoped integer handle for a ticker symbol, assigned in the `R` (Stock Directory) messages at session open. It replaces the 8-byte ASCII ticker in all subsequent messages, reducing message size and eliminating string comparisons in the hot path.

```cpp
// Populated once at session start from 'R' messages
std::array<Instrument, 65536> locate_table; // direct index, O(1)

// Usage in the hot path — zero branching, zero string ops
const Instrument& inst = locate_table[msg->stock_locate];
```

This is the ITCH equivalent of a symbol ID — never look up by string in the critical path.

---

## 9. Network Architecture in a Collocated HFT Environment

### Kernel Bypass
In production, the ITCH feed is received via **kernel bypass networking**:

- **Solarflare OpenOnload** or **DPDK** (Data Plane Development Kit)
- The NIC DMA's packets directly into application memory via a **poll-mode driver** (PMD); no system calls, no kernel scheduler involvement
- Latency from wire to application: **~500 ns–2 µs** vs. **~5–50 µs** with standard sockets

### CPU Affinity & NUMA
```
NIC (PCIe slot on NUMA node 0)
  └─→ RX queue pinned to core 2 (NUMA node 0, isolated)
        └─→ ITCH decode thread (same core, spinning)
              └─→ Lock-free SPSC queue
                    └─→ Strategy thread (core 4, isolated)
```

- Cores are isolated with `isolcpus` in the kernel command line
- The decode thread **spin-polls** the NIC RX ring — no blocking, no sleep
- Inter-thread communication via **lock-free SPSC ring buffers** (e.g., Disruptor pattern or `std::atomic`-based ring)
- Memory allocated from **huge pages** (2 MB or 1 GB) to eliminate TLB misses

### Feed Redundancy
NASDAQ publishes ITCH on **two independent multicast feeds** (Feed A and Feed B) with the same content. An HFT firm will receive both simultaneously and use whichever arrives first, using the sequence number to deduplicate. This is called **feed arbitrage** and shaves 1–5 µs of jitter-induced latency risk.

---

## 10. Business-Critical Features for HFT Strategies

### 10.1 Order Flow Toxicity Detection
By tracking `E` (execution) messages against outstanding book orders, you can compute **fill rates per price level** and model whether the flow at a given price is informed (toxic) or uninformed. This feeds into adverse selection models for market-making strategies.

### 10.2 Hidden Order Inference
ITCH only shows **displayed orders**. `C` (Order Executed with Price) messages — executions at a price different from any displayed limit — reveal the existence of **hidden/reserve orders** (iceberg orders). Tracking the frequency and size of these inferred hidden fills informs liquidity models significantly.

### 10.3 Queue Position Modeling
Because ITCH shows every add/cancel/execute in sequence-number order (i.e., exchange-sequenced order), you can reconstruct the **precise FIFO queue position** of every resting order. This is invaluable for:
- Estimating your own fill probability if you were to post at a given price
- Detecting when a competitor's order is near the front of the queue (informative for alpha signals)

### 10.4 NOII & Cross Auction Trading
The `I` (NOII) message during the opening and closing cross gives real-time imbalance data. A strategy can:
1. Track the paired interest vs. imbalance quantity
2. Estimate the clearing price from the near/far collar
3. Submit MOO/MOC orders via OUCH (NASDAQ's order entry protocol) to participate in the cross at favorable prices

### 10.5 Latency Benchmarking
The ITCH timestamp (exchange-side) combined with your NIC hardware timestamp gives **one-way latency** from NASDAQ's matching engine output to your NIC. This is typically 1–4 µs in colocation and is used to:
- Detect feed degradation (useful for risk management)
- Correlate with your own order submission latencies (OUCH round-trip)

---

## 11. Implementation Gotchas

| Pitfall | Description | Mitigation |
|---|---|---|
| **`U` message new ref** | The new `order_ref_num` in a replace has no relation to the old one — do not assume monotonicity | Always erase old, insert new |
| **`P` messages and the book** | Non-cross trades (`P`) are off-book prints; they must NOT modify your limit book | Guard with `msg_type != 'P'` |
| **`B` (Broken Trade)** | A busted trade must reverse your executed volume and P&L accounting | Maintain a trade log keyed by `match_number` |
| **Locate reuse** | `stock_locate` values are session-scoped and may be reused across sessions | Clear and repopulate on each `S` = `'O'` (Start of Messages) |
| **Endianness** | All multi-byte integers are big-endian | Always `bswap` on x86-64; use a validated decode layer |
| **Sequence gaps on failover** | Switching from feed A to feed B mid-session can introduce apparent gaps | Maintain independent sequence state per feed; merge by sequence number |
| **Stock Directory completeness** | `R` messages span multiple packets; don't begin book building until you've processed all of them | Buffer `R` messages; start processing only after `S` = `'S'` (System Start) |

---

## 12. Summary of Design Principles

ITCH 5.0 represents a specific point in the design space: **maximum throughput, minimal overhead, zero tolerance for latency jitter**, at the cost of reliability guarantees. Every design decision — fixed-width fields, UDP multicast, stock locates, big-endian integers, 48-bit timestamps — reflects the operational reality of a modern equity exchange where thousands of messages per second per symbol must be processed with single-digit microsecond budgets. For a C++ HFT system, the protocol is close to ideal: it maps directly to structs, requires no allocations in the hot path, and provides all the information needed to maintain a pixel-perfect replica of the NASDAQ order book in real time.

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Provide a thorough description of the ITCH 5.0 (NASDAQ Market Data Feed) protocol in a high-frequency trading environment. This description is intended for a computer science expert fluent in C++. Describe the important technical and business features of the protocol.
