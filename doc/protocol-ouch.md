# OUCH: NASDAQ Order Entry Protocol — A Deep Technical Reference

## 1. Overview and Context

OUCH is a binary, session-layer order entry protocol developed by NASDAQ for direct market access (DMA). It is the primary mechanism through which **sponsored participants, broker-dealers, and proprietary HFT firms** submit, modify, and cancel orders on NASDAQ equity markets. Its design philosophy is ruthlessly utilitarian: minimize wire bytes, minimize parsing overhead, maximize determinism. Every design decision subordinates human readability and generality to **microsecond-level latency budgets**.

OUCH sits above a TCP connection (almost universally over a co-location cross-connect) and below an application-level order management system (OMS). The session layer is provided by **SoupBinTCP** (NASDAQ's framing protocol), which handles sequencing, heartbeats, and login — OUCH itself is purely a message payload specification.

---

## 2. Transport and Session Layer: SoupBinTCP

Before examining OUCH messages themselves, the framing layer must be understood.

### SoupBinTCP Frame Structure

Every OUCH message is wrapped in a SoupBinTCP frame:

```
+------------------+------------------+--------------------+
|  Packet Length   |  Packet Type     |  Payload           |
|  2 bytes (BE)    |  1 byte (ASCII)  |  variable          |
+------------------+------------------+--------------------+
```

The total on-wire frame is `2 + 1 + len(payload)` bytes. The length field encodes `1 + len(payload)` (i.e., it counts the type byte). Key packet types:

| Type Byte | Direction        | Meaning                          |
|-----------|------------------|----------------------------------|
| `'S'`     | Server → Client  | Sequenced data (server-side msgs)|
| `'U'`     | Client → Server  | Unsequenced data (order messages)|
| `'H'`     | Bidirectional    | Heartbeat (sent if idle >1s)     |
| `'R'`     | Client → Server  | Login request                    |
| `'A'`     | Server → Client  | Login accepted                   |
| `'J'`     | Server → Client  | Login rejected                   |

**Login** carries a username (6 bytes), password (10 bytes), requested session (10 bytes), and sequence number (20 bytes). The sequence number enables **gap recovery**: a client reconnecting after a drop can request replay from the last confirmed sequence number, receiving missed execution reports before new live messages. This is critical for state reconciliation in an HFT OMS.

### TCP Considerations in HFT

OUCH mandates TCP. In co-location environments firms typically tune the kernel socket layer aggressively:

```cpp
// Typical kernel bypass / socket tuning in a co-lo environment
int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

int flag = 1;
setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)); // Disable Nagle

int sndbuf = 1 << 20; // 1 MB send buffer
setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

// On Linux with DPDK or kernel bypass (Solarflare/OpenOnload):
// onload_set_stackname("hft_stack", ONLOAD_ALL_THREADS);
```

`TCP_NODELAY` is non-negotiable — Nagle's algorithm would batch small OUCH messages (typically 40–80 bytes) and could add hundreds of microseconds of latency.

---

## 3. Message Catalog

OUCH defines two message directions: **inbound** (client → exchange) and **outbound** (exchange → client).

### 3.1 Inbound Messages (Client → Exchange)

#### Enter Order (`'O'`) — The Core Message

```
Offset  Length  Field
------  ------  -----
0       1       Message Type ('O')
1       14      Order Token         (client-assigned, left-justified, space-padded)
15      1       Buy/Sell Indicator  ('B', 'S', 'T'=short sell, 'E'=short sell exempt)
16      4       Shares              (uint32, big-endian)
20      8       Stock               (ASCII, left-justified, space-padded)
28      4       Price               (uint32, implicit 4 decimal places, big-endian)
32      4       Time In Force       (uint32, seconds; 0=DAY, 99998=IOC, 99999=GTX)
36      4       Firm               (ASCII, 4-byte MPID)
40      1       Display             ('Y'=displayed, 'N'=non-displayed, 'P'=post-only, etc.)
41      4       Capacity            ('A'=agency, 'P'=principal, 'R'=riskless principal, etc.)
45      1       Intermarket Sweep   ('Y'=ISO, 'N'=non-ISO)
46      4       Minimum Quantity    (uint32)
50      1       Cross Type          ('N'=not a cross order, etc.)
51      1       Customer Type       (institutional/retail indicator)
```

Total wire size: **52 bytes** (version-dependent; later versions add fields).

The **Order Token** is perhaps the most operationally significant field: it is a 14-character client-defined identifier that threads through all downstream messages. In a well-designed HFT system, the token encodes enough information to route the execution report back to the originating strategy without a hash table lookup:

```cpp
// Compact token encoding: strategy_id (2) + side (1) + price_tick (4) + nonce (7)
// Avoids std::unordered_map on the hot path
struct alignas(16) OrderToken {
    char data[14];
    char pad[2]; // align to 16 bytes for SIMD comparison

    void encode(uint16_t strategy, char side, uint32_t price_tick, uint32_t nonce) {
        // Pack fields into ASCII-safe base62 or raw binary
        // Must be printable ASCII per spec
        std::memcpy(data, encode_base62(strategy, side, price_tick, nonce), 14);
    }
};
```

#### Replace Order (`'U'`)

```
Offset  Length  Field
0       1       Message Type ('U')
1       14      Existing Order Token
15      14      Replacement Order Token
29      4       Shares
33      4       Price
37      4       Time In Force
41      1       Display
42      4       Intermarket Sweep (flags)
46      4       Minimum Quantity
```

**Replace is not cancel-and-new.** This is a critical distinction. OUCH Replace is an **atomic modify** at the exchange matching engine. It preserves queue priority under NASDAQ's rules if only the quantity is *decreased* (and some other conditions). A cancel followed by a new order unconditionally loses queue priority — a significant alpha leakage in a maker strategy. HFT systems must carefully model when to Replace vs. cancel-replace:

```cpp
enum class ModifyStrategy {
    REPLACE,          // Preserve priority if only qty reduced
    CANCEL_NEW,       // Mandatory for price changes
    CANCEL_ONLY       // Passive pull, no re-entry
};

ModifyStrategy choose_modify(const Order& existing, const OrderUpdate& desired) {
    if (desired.price != existing.price)
        return ModifyStrategy::CANCEL_NEW;  // Price change loses priority anyway
    if (desired.qty < existing.leaves_qty)
        return ModifyStrategy::REPLACE;     // Qty reduction may preserve priority
    if (desired.qty > existing.leaves_qty)
        return ModifyStrategy::CANCEL_NEW;  // Qty increase loses priority
    return ModifyStrategy::CANCEL_ONLY;
}
```

#### Cancel Order (`'X'`)

```
Offset  Length  Field
0       1       Message Type ('X')
1       14      Order Token
15      4       Shares (0 = cancel all remaining)
```

Partial cancels are supported: setting `Shares` to a non-zero value less than the outstanding quantity reduces the order size. Again, partial cancel preserving remaining shares maintains queue position for the residual quantity.

---

### 3.2 Outbound Messages (Exchange → Client)

#### System Event (`'S'`)
Signals start-of-day (`'S'`), end-of-day (`'E'`), etc. Used to gate strategy startup/shutdown.

#### Order Accepted (`'A'`)
Confirms the exchange has booked the order. Contains the exchange-assigned **Order Reference Number** (uint64) — a monotonically increasing integer that the exchange uses in subsequent messages. The client must maintain a bidirectional map between its Order Token and the exchange's Order Reference Number:

```cpp
// Lock-free token↔refnum mapping — critical path data structure
// Use open-addressing hash map with power-of-2 size for bitmask indexing
struct OrderMap {
    static constexpr size_t CAPACITY = 1 << 14; // 16384 slots, ~98% fill for 16K orders
    static constexpr size_t MASK = CAPACITY - 1;

    struct Slot {
        std::atomic<uint64_t> ref_num{0};
        char token[14];
        uint8_t state; // EMPTY, LIVE, PENDING_CANCEL
        uint8_t pad[1];
    };

    alignas(64) Slot slots[CAPACITY]; // cache-line aligned
};
```

Accepted also echoes back all order parameters, allowing the client to confirm no field was silently clamped or modified by the exchange's risk filters.

#### Order Replaced (`'U'`)
Confirms a replace. Critically, it includes a **new** Order Reference Number. The old reference is dead. Failure to update the local mapping here is a common source of bugs — a subsequent cancel referencing the old token will be rejected.

#### Order Canceled (`'C'`)
Includes a **Reason** byte:

| Reason | Meaning |
|--------|---------|
| `'U'`  | User-requested cancel |
| `'I'`  | Immediate-or-cancel expired |
| `'T'`  | Timeout (TIF expired) |
| `'D'`  | Supervisory cancel (by exchange) |
| `'E'`  | Closed for trading |

The `'D'` (supervisory) cancel deserves special attention: it means the exchange's risk engine forcibly pulled the order. This can happen during market halts, volatility pauses (LULD), or when the firm's gross notional limit is breached. A well-hardened OMS treats `'D'` as a risk event requiring immediate position reconciliation.

#### Order Executed (`'E'`)

```
Offset  Length  Field
0       1       Message Type ('E')
1       8       Timestamp           (uint64, nanoseconds since midnight)
9       14      Order Token
23      8       Order Reference Number
31      4       Executed Shares     (uint32)
35      4       Execution Price     (uint32, 4 implicit decimals)
39      1       Liquidity Flag
40      8       Match Number        (uint64, unique execution ID)
```

The **Liquidity Flag** determines fee/rebate:

| Flag | Meaning |
|------|---------|
| `'A'` | Added liquidity (maker) — typically receives rebate |
| `'R'` | Removed liquidity (taker) — pays fee |
| `'X'` | Cross trade |
| `'C'` | Continuous book cross |
| `'7'` | Retail-designated execution |

This is a **primary revenue signal** for market-making strategies. The real-time fee/rebate tracking feeds directly into PnL attribution.

The **timestamp** is exchange-assigned in nanoseconds and reflects the matching engine's clock. Combined with the client's local send timestamp (captured via `clock_gettime(CLOCK_REALTIME, ...)` or RDTSC), this enables **round-trip latency measurement** and detection of queue position degradation.

#### Order Executed with Price (`'F'`)
Variant of `'E'` where the execution price differs from the order's limit price (e.g., price improvement). Contains an additional **Reference Price** field.

#### Broken Trade (`'B'`)
The exchange nullifies a previously reported execution. This is catastrophic for position tracking. A robust OMS handles this by:
1. Reversing the position immediately
2. Logging the match number for reconciliation
3. Alerting risk management
4. Potentially halting the affected strategy

```cpp
void OuchSession::on_broken_trade(const BrokenTradeMsg& msg) {
    auto it = execution_map_.find(msg.match_number);
    if (it == execution_map_.end()) {
        risk_alert("BROKEN TRADE for unknown match: " + std::to_string(msg.match_number));
        halt_strategy(StrategyHaltReason::EXECUTION_INTEGRITY_FAILURE);
        return;
    }
    position_manager_.reverse_execution(it->second);
    execution_map_.erase(it);
}
```

---

## 4. Wire Encoding and Parsing

### Binary Layout Rules
- All multi-byte integers are **big-endian** (network byte order)
- Prices are **fixed-point integers** with 4 implicit decimal places: price `$10.2500` → `102500`
- Strings are **ASCII, left-justified, space-padded** (not null-terminated)
- No TLV, no length-prefixed strings within messages — all fields are fixed-offset, fixed-width

This fixed-layout design means parsing is a single `memcpy` into a packed struct, or better, a **reinterpret_cast** directly from the receive buffer:

```cpp
#pragma pack(push, 1)
struct OuchExecutionMsg {
    char     msg_type;          // 'E'
    uint64_t timestamp;         // ns since midnight, big-endian
    char     order_token[14];
    uint64_t order_ref_num;     // big-endian
    uint32_t executed_shares;   // big-endian
    uint32_t execution_price;   // big-endian, 4 decimal places
    char     liquidity_flag;
    uint64_t match_number;      // big-endian
};
#pragma pack(pop)

// Zero-copy parse — buffer is the receive ring buffer
const OuchExecutionMsg* msg =
    reinterpret_cast<const OuchExecutionMsg*>(recv_buf + 3); // skip 2-byte len + 1-byte type

uint32_t price_raw = __builtin_bswap32(msg->execution_price);
double price = price_raw / 10000.0; // avoid in hot path; keep as fixed-point
uint32_t shares = __builtin_bswap32(msg->executed_shares);
```

Note: `__builtin_bswap32`/`__builtin_bswap64` compile to `BSWAP` on x86-64 — a single-cycle instruction. Avoid `ntohl`/`ntohll` which may have function call overhead in non-inlined builds.

### Kernel Bypass Receive Path

In a serious HFT deployment, the receive path uses **kernel bypass** (Solarflare `ef_vi`, Mellanox `VMA`, or DPDK) to eliminate system call overhead:

```cpp
// ef_vi poll loop — no system calls after initial setup
while (true) {
    ef_event events[EF_VI_TRANSMIT_BATCH];
    int n = ef_eventq_poll(&vi_, events, EF_VI_TRANSMIT_BATCH);
    for (int i = 0; i < n; ++i) {
        if (EF_EVENT_TYPE(events[i]) == EF_EVENT_TYPE_RX) {
            uint8_t* pkt = get_rx_buffer(EF_EVENT_RX_RQ_ID(events[i]));
            // pkt points to Ethernet frame; skip to TCP payload
            dispatch_ouch(pkt + ETH_IP_TCP_HDR_LEN);
        }
    }
}
```

---

## 5. State Machine

The client must maintain a per-order state machine. Transitions are driven by inbound OUCH messages:

```
           ┌─────────────────────────────────────────┐
           │          ENTER ORDER sent                │
           ▼                                         │
       [PENDING_NEW]                                 │
       /           \                                 │
  Accepted(A)    Rejected(J)                        │
      │               │                             │
  [LIVE]          [DEAD]                            │
  /   |   \                                         │
Exec  Replace  Cancel                               │
 │      │        │                                  │
[LIVE] [PENDING_REPLACE] [PENDING_CANCEL]           │
        │                    │                      │
    Replaced(U)           Canceled(C)               │
        │                    │                      │
    [LIVE*]              [DEAD]                     │
    (new token)                                     │
```

A key nuance: between sending `Cancel` and receiving `Canceled` or `Executed`, the order is in a **race window**. An execution arriving after a cancel request is valid and must be processed:

```cpp
// Correct handling of the cancel/exec race
void OuchSession::on_executed(const OuchExecutionMsg& msg) {
    auto& order = order_map_.get(msg.order_token);
    
    // Even if we sent a cancel, this execution is real — process it
    if (order.state == OrderState::PENDING_CANCEL ||
        order.state == OrderState::LIVE) {
        position_manager_.add_fill(order.symbol, order.side,
                                   msg.executed_shares, msg.execution_price);
        order.leaves_qty -= msg.executed_shares;
        if (order.leaves_qty == 0)
            order.state = OrderState::DEAD;
        // Do NOT transition to DEAD on PENDING_CANCEL here — wait for Canceled msg
    }
}
```

---

## 6. Risk Controls and Regulatory Dimensions

### Pre-Trade Risk (Fat Finger / Gross Notional)
The exchange's risk engine enforces limits configured per MPID. Client-side, HFT firms layer their own pre-trade checks before the message even hits the NIC:

```cpp
bool RiskGate::check_enter_order(const OrderRequest& req) {
    // All checks must be branchless or near-branchless on hot path
    uint64_t notional = (uint64_t)req.price * req.shares; // fixed-point, no float

    // Atomic load — position updated by execution thread
    int32_t net_pos = net_position_[req.symbol_idx].load(std::memory_order_relaxed);
    int32_t new_pos = req.side == Side::BUY ? net_pos + req.shares
                                            : net_pos - req.shares;

    return (notional <= per_order_notional_limit_)
        && (std::abs(new_pos) <= symbol_position_limit_[req.symbol_idx])
        && (gross_notional_.load(std::memory_order_relaxed) + notional
            <= firm_gross_notional_limit_);
}
```

### Regulation SHO (Short Sale)
The `Buy/Sell Indicator` field distinguishes `'S'` (short sell) from `'T'` (short sell exempt). Firms must accurately classify orders — mismarking is a regulatory violation (FINRA Rule 4560). The OMS must track locate availability and current inventory before marking an order short.

### Intermarket Sweep Orders (ISO)
Setting `Intermarket Sweep = 'Y'` tells NASDAQ the sending firm has already routed to all protected quotes at better prices on other exchanges, fulfilling Reg NMS Rule 611 (Order Protection). ISOs bypass NASDAQ's own routing and execute immediately against the local book. In HFT, ISOs are used for **aggressive cross-venue strategies** where the firm's smart order router handles Reg NMS compliance internally. Using ISO incorrectly is a serious regulatory violation.

### LULD (Limit Up/Limit Down) Handling
NASDAQ will reject or cancel orders outside the LULD price bands with a specific reject reason. The OMS must consume the NASDAQ HALT feed (via ITCH 5.0) to maintain current bands and pre-validate prices, since the reject round-trip wastes 10–50µs in co-location:

```cpp
// Inline price band check before sending Enter Order
bool is_within_luld(uint32_t price_fixed4, uint8_t symbol_idx) {
    // Band updated by separate ITCH processing thread
    // Read with relaxed ordering — stale by microseconds is acceptable
    uint32_t lower = luld_lower_[symbol_idx].load(std::memory_order_relaxed);
    uint32_t upper = luld_upper_[symbol_idx].load(std::memory_order_relaxed);
    return price_fixed4 >= lower && price_fixed4 <= upper;
}
```

---

## 7. Performance Engineering

### Message Batching vs. Latency
SoupBinTCP permits multiple OUCH messages in a single TCP segment. However, in latency-sensitive paths, **each order is sent immediately** — no Nagle (already disabled), no application-level batching. Throughput-oriented paths (e.g., bulk cancel-on-disconnect) may batch.

### Token Generation: Avoiding Allocation
The 14-byte order token must be unique per session and efficiently reversible to internal state. A common pattern is a **pool-allocated token ring**:

```cpp
class TokenAllocator {
    static constexpr uint32_t POOL_SIZE = 1 << 16;
    alignas(64) std::array<char[14], POOL_SIZE> pool_;
    std::atomic<uint32_t> counter_{0};
public:
    const char* next() {
        uint32_t idx = counter_.fetch_add(1, std::memory_order_relaxed) & (POOL_SIZE - 1);
        // Encode idx into ASCII base-36 in pool_[idx]
        encode_token(pool_[idx], session_id_, idx);
        return pool_[idx];
    }
};
```

### Timestamping for Latency Measurement
```cpp
// RDTSC-based send timestamp — ~20 cycles, no system call
inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Capture immediately before write() / ef_vi send
order.send_tsc = rdtsc();

// On Accepted/Executed, compute round-trip
uint64_t rtt_cycles = rdtsc() - order.send_tsc;
double rtt_ns = rtt_cycles / tsc_ghz_; // calibrated at startup
```

### Cancel-on-Disconnect (CoD)
OUCH supports a **Cancel on Disconnect** flag in the login handshake or via a dedicated message. When the TCP session drops, NASDAQ automatically cancels all resting orders for that session. This is a critical safety feature: without it, a connectivity failure leaves orphaned orders on the book. Every HFT session should have CoD enabled.

---

## 8. Comparison with Competing Protocols

| Feature | OUCH (NASDAQ) | FIX (generic) | BATS PITCH/BOE | NYSE Pillar |
|---|---|---|---|---|
| Encoding | Binary | ASCII/Tag-Value | Binary | Binary |
| Typical msg size | 40–80 bytes | 200–500 bytes | 40–70 bytes | 40–80 bytes |
| Parsing complexity | Trivial (fixed offsets) | High (tag scanning) | Trivial | Trivial |
| Session layer | SoupBinTCP | FIXT | BOE session | Pillar session |
| Gap recovery | Yes (sequence replay) | Varies | Yes | Yes |
| Latency (co-lo) | ~1–5 µs RTT | ~50–200 µs RTT | ~1–5 µs RTT | ~1–5 µs RTT |

FIX is essentially never used on the hot path in HFT — it exists for back-office and less latency-sensitive institutional flow.

---

## 9. Operational Considerations

- **Session sequencing**: Every server-originated message carries a sequence number (provided by SoupBinTCP). The client must track the last received sequence and detect gaps, triggering a reconnect-with-replay if a gap is detected. Gaps indicate packet loss or session issues — extremely rare over a co-location cross-connect but not impossible.
- **Heartbeat discipline**: SoupBinTCP heartbeats (type `'H'`) must be responded to within the configured interval (typically 1 second). Missing heartbeats cause the exchange to terminate the session. In practice, HFT systems send heartbeats from a dedicated low-priority thread, never the order path.
- **Fat binary vs. shared library**: In production, the OUCH session handler is typically compiled as a **static library** linked directly into the trading process with LTO (link-time optimization) to eliminate call overhead across translation units and enable cross-module inlining of the hot path.
- **CPU isolation and IRQ affinity**: The OUCH receive thread is pinned to an isolated core (`isolcpus` kernel boot parameter) with NIC interrupts affinitized to that core, eliminating scheduler jitter.

---

## Summary

OUCH is elegantly minimal: it is essentially a **binary RPC layer over TCP** with a small, fixed message vocabulary, fixed-width fields, and no schema overhead. Its design reflects the economics of HFT — every nanosecond of parsing, every unnecessary byte on the wire, and every kernel context switch is real money. For a C++ engineer building an OUCH client, the key engineering challenges are: **zero-copy receive**, **lock-free order state management**, **correct handling of the cancel/execute race**, **accurate pre-trade risk in the nanosecond budget**, and **robust gap detection and recovery** for session reliability.

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Provide a thorough description of the OUCH (NASDAQ Order Entry Protocol) protocol in a high-frequency trading environment. This description is intended for a computer science expert fluent in C++. Describe the important technical and business features of the protocol.
