# High-Frequency Trading Protocols

High-frequency trading (HFT) infrastructure is built on a stack of highly specialized, latency-optimized protocols. Each operates at a distinct layer of the trading lifecycle — from market data dissemination to order entry to wire encoding. Understanding them requires appreciating the extreme engineering constraints: sub-microsecond processing budgets, deterministic latency, zero garbage tolerance, and kernel-bypass networking.

---

## 1. FIX 5.0 (Financial Information eXchange)

### Overview
FIX is the lingua franca of inter-firm financial messaging. Version 5.0 (released ~2006, extended by FIX 5.0 SP2 in 2009) decoupled the **session layer** from the **application layer** for the first time, introducing the **FIXT 1.1** transport specification.

### Wire Format
FIX 5.0 uses a **tag=value** ASCII encoding by default:

```
8=FIXT.1.1|9=176|35=D|49=CLIENT1|56=EXCHANGE|34=12|52=20240101-09:30:00.000|
11=ORD-001|55=AAPL|54=1|38=100|40=2|44=185.50|59=0|10=142|
```

Each field is a `Tag=Value` pair delimited by SOH (`\x01`). Tag 35 is the **MsgType** — the most critical routing field. The full message is framed by a **checksum** (Tag 10) and **body length** (Tag 9).

### Session Layer (FIXT 1.1)
The decoupled session layer manages:
- **Logon/Logout** (MsgType `A` / `5`)
- **Sequence number synchronization** (Tag 34: `MsgSeqNum`)
- **Heartbeats** (MsgType `0`) at configurable intervals
- **ResendRequest** (MsgType `2`) and **SequenceReset** (MsgType `4`) for gap recovery
- **TestRequest** / **Heartbeat** pairs for liveness detection

Sequence numbers are monotonically increasing per session and are the **primary recovery mechanism**. Gaps trigger retransmission requests, making FIX inherently stateful and TCP-dependent.

### Key Application Messages
| MsgType | Name | Purpose |
|---|---|---|
| `D` | NewOrderSingle | Submit a new order |
| `F` | OrderCancelRequest | Cancel an existing order |
| `G` | OrderCancelReplaceRequest | Modify price/qty atomically |
| `8` | ExecutionReport | Ack, fill, reject notification |
| `9` | OrderCancelReject | Cancel/replace rejection |
| `V` / `W` | MarketDataRequest / Snapshot | Market data subscription |
| `X` | MarketDataIncrementalRefresh | Incremental updates |

### FIX Performance Characteristics
FIX's ASCII encoding is its Achilles heel in HFT:
- **Parsing overhead**: Every field requires `atoi`/`atof` + tag lookup — typically thousands of CPU cycles per message
- **Verbosity**: A NewOrderSingle for an equity order runs ~200–400 bytes
- **TCP dependency**: Retransmission and head-of-line blocking are antithetical to HFT
- **Garbage generation**: In JVM implementations, tag-value parsing creates string allocations

In practice, **FIX is rarely used on the hot path in HFT**. It is used for:
- **Drop copy** (post-trade regulatory reporting)
- **Prime broker connectivity**
- **Give-up and allocation messages**
- **Low-frequency algo order management**

Most venues expose a binary protocol (OUCH, BOE, etc.) for their co-location clients and reserve FIX for external or less latency-sensitive connections.

### FIXP (FIX Performance Protocol)
The FIX Trading Community introduced **FIXP** as a high-performance session layer designed for use with **SBE** (discussed below). FIXP supports:
- **UDP multicast** (no TCP retransmission)
- **Unsequenced** and **sequenced** channels
- **Negotiation-based** session setup
- **Idempotent flow** mode (exchange assigns IDs, tolerates retransmissions)

---

## 2. OUCH (NASDAQ Order Entry Protocol)

### Overview
OUCH is NASDAQ's **binary, low-latency order entry protocol** used by co-location clients for direct market access. It operates over **SoupBinTCP** (a lightweight framing protocol over TCP) or **SOUP-UDP** for specific use cases. OUCH is a pure binary protocol — no ASCII, no tags.

### Wire Format
Messages are **fixed-length binary structs**, packed with no padding (or explicit padding for alignment). The message begins with a **Packet Type** byte, followed by fields in a predefined order:

```
[Length: 2 bytes][Packet Type: 1 byte][Message Fields: N bytes]
```

A **Enter Order** (Packet Type `O`) message looks like:

```c
struct OUCHEnterOrder {
    char     packet_type;       // 'O'
    char     order_token[14];   // Client-assigned order ID
    char     buy_sell;          // 'B' or 'S'
    uint32_t shares;            // Big-endian
    char     stock[8];          // Left-justified, space-padded
    uint32_t price;             // Price * 10000 (fixed-point)
    uint32_t time_in_force;     // Seconds, or 0=day, 99998=IOC, 99999=GTX
    char     firm[4];           // MPID
    char     display;           // 'Y'=visible, 'N'=hidden, 'P'=post-only
    uint64_t capacity;          // Customer/Principal/etc.
    char     iso_eligibility;
    char     min_qty[4];
    char     cross_type;
    char     customer_type;
};
```

### Key Messages

| Type | Direction | Purpose |
|---|---|---|
| `O` | Client→Exchange | Enter Order |
| `U` | Client→Exchange | Replace Order (cancel-replace) |
| `X` | Client→Exchange | Cancel Order |
| `A` | Exchange→Client | System Event |
| `S` | Exchange→Client | Order Accepted |
| `U` | Exchange→Client | Order Replaced |
| `C` | Exchange→Client | Order Canceled |
| `E` | Exchange→Client | Order Executed |
| `B` | Exchange→Client | Broken Trade |
| `J` | Exchange→Client | Order Rejected |

### Technical Highlights

**Fixed-point arithmetic**: Prices are represented as integers (e.g., price × 10,000), eliminating all floating-point arithmetic on the critical path. This is crucial for deterministic behavior in both hardware and software implementations.

**Order Token**: The 14-byte `order_token` is a **client-managed opaque identifier** that the exchange reflects back in all responses. This allows O(1) lookup via hash map keyed on the token, without any exchange-assigned ID round-trip before cancel/replace.

**Cancel-Replace atomicity**: The `Replace` message (`U`) is an atomic cancel+resubmit — the exchange guarantees that if the replacement succeeds, the old order is dead. There is no race window between a manual cancel and a new submit.

**SoupBinTCP framing**: OUCH sits on top of SoupBinTCP, which provides:
- 2-byte length-prefixed framing (enabling zero-copy reads with a single `recv` call sized to the length header)
- Sequence numbers for gap detection
- Heartbeats (`H`) and Login/Logout

**Latency profile**: NASDAQ co-location OUCH round-trip latencies are typically **in the range of 60–200 nanoseconds** for wire-to-wire in the matching engine, measured from when the packet arrives at the NASDAQ switch.

---

## 3. ITCH 5.0 (NASDAQ Market Data Feed)

### Overview
ITCH 5.0 is NASDAQ's **inbound market data protocol** — it is **unidirectional**, **multicast UDP**, and carries the **full order book** as a stream of events. It does not support any form of acknowledgment or request/response. ITCH is the canonical example of a **feed handler** protocol.

### Transport
ITCH runs over **MoldUDP64**, a lightweight UDP framing layer:

```
[Session: 10 bytes][Sequence Number: 8 bytes][Message Count: 2 bytes]
[Message Length: 2 bytes][Message: N bytes] ...
```

A single UDP datagram can carry **multiple ITCH messages** (up to MTU). MoldUDP64 provides:
- **64-bit sequence numbers** (hence "64") — enabling detection of gaps without state complexity
- **Retransmission via a separate TCP channel** (MOLDUDP RETRANSMIT): upon detecting a gap, a subscriber can issue a `Request` to the retransmit server to replay missed packets
- **Session identifier**: a 10-byte ASCII session ID that changes daily, preventing stale data confusion on reconnect

### Message Taxonomy

**System Events**:
- `S` — System Event (e.g., `O`=Start of Messages, `C`=End of Messages, `M`=Start of Market Hours)

**Stock Directory / Reference Data**:
- `R` — Stock Directory (ticker, market category, financial status, lot size, round lots only flag)
- `H` — Stock Trading Action (halt, resume, quotation)
- `Y` — Reg SHO Short Sale Price Test Restricted Indicator
- `L` — Market Participant Position

**Order Book Events**:
- `A` — Add Order (no MPID) — `[timestamp: 8][order_ref: 8][buy_sell: 1][shares: 4][stock: 8][price: 4]`
- `F` — Add Order with MPID
- `E` — Order Executed (partial fill)
- `C` — Order Executed with Price (price-override fills, e.g., odd lots)
- `X` — Order Cancel (partial cancel)
- `D` — Order Delete (full cancel)
- `U` — Order Replace (cancel+reinsert at new price/qty)

**Trade Messages**:
- `P` — Non-Cross Trade (off-book trade)
- `Q` — Cross Trade (opening/closing)
- `B` — Broken Trade

**NOII (Net Order Imbalance Indicator)**:
- `I` — NOII message: published in the pre-open and close auctions; includes paired shares, imbalance shares, imbalance direction, far/near/current reference prices

### Order Book Reconstruction

Rebuilding the full limit order book from ITCH requires maintaining a **hash map keyed on `order_reference_number`** (a 64-bit monotonically increasing integer assigned by NASDAQ). The algorithm:

```
on Add Order (A/F):
    book[order_ref] = {side, shares, price}
    price_level[side][price] += shares

on Order Executed (E):
    book[order_ref].shares -= executed_shares
    price_level[side][price] -= executed_shares

on Order Cancel (X):
    book[order_ref].shares -= cancelled_shares
    price_level[side][price] -= cancelled_shares

on Order Delete (D):
    price_level[side][book[order_ref].price] -= book[order_ref].shares
    delete book[order_ref]

on Order Replace (U):
    old = book[orig_order_ref]
    price_level[old.side][old.price] -= old.shares
    delete book[orig_order_ref]
    book[new_order_ref] = {old.side, new_shares, new_price}
    price_level[old.side][new_price] += new_shares
```

### Performance Engineering on ITCH

**Timestamp precision**: All timestamps are **nanoseconds since midnight** in a 64-bit field. This allows direct subtraction for latency measurement without conversion.

**Zero-copy parsing**: Because messages are fixed-size and typed by a single leading byte, a typical ITCH parser is a `switch` on byte 0, followed by a `reinterpret_cast<MessageType*>(buf)`. No allocation, no dynamic dispatch beyond the initial branch.

**SIMD price level aggregation**: The `price_level` map is often implemented as a **sparse array** for actively traded symbols (prices cluster tightly around mid), allowing SIMD batch summation for BBO computation.

**CPU affinity & NUMA**: Feed handler threads are pinned to cores on the same NUMA node as the NIC's interrupt affinity. On Solarflare/Mellanox NICs with kernel-bypass (OpenOnload / DPDK), the packet is DMA'd directly into a pre-allocated ring buffer visible to the user-space application.

---

## 4. PITCH (CBOE/BATS Market Data Protocol)

### Overview
PITCH is the market data feed protocol developed by **BATS Global Markets** (now **CBOE**). It is conceptually similar to ITCH but has several architectural and message-level differences. PITCH covers **equities, options, and futures** across CBOE's family of exchanges (BZX, BYX, EDGX, EDGA, C2, CFE).

### Transport
PITCH uses **UDP multicast** with a **GRP (Gap Request Protocol)** for retransmission, or alternatively a **Spin Server** (TCP) for snapshot-based recovery. The spin server can replay a configurable window of messages, enabling subscribers to bootstrap from a mid-session start.

### Message Format

PITCH messages begin with a **2-byte length** and a **1-byte message type**:

```
[Length: 2][Type: 1][Fields...]
```

A **Unit Header** (analogous to MoldUDP64's envelope) precedes each UDP datagram:

```
[Length: 2][Count: 1][Unit: 1][Sequence: 4]
```

Note the **32-bit sequence number** (vs. ITCH/MoldUDP64's 64-bit) — at BATS's peak volumes, 32-bit sequences roll over sufficiently slowly to not be problematic in practice, but this is a notable limitation vs. ITCH.

### Core Message Types

| Type | Name | Key Fields |
|---|---|---|
| `0x20` | Add Order (Short) | Order ID (8B), Side, Qty (2B), Symbol (6B), Price (10B) |
| `0x21` | Add Order (Long) | Order ID (8B), Side, Qty (4B), Symbol (8B), Price (10B) |
| `0x22` | Add Order (Expanded) | Includes participant ID |
| `0x23` | Execute Order | Order ID, Exec Qty, Exec ID |
| `0x24` | Execute Order at Price | Order ID, Exec Qty, Exec ID, Price (for price-override trades) |
| `0x25` | Reduce Size (Short) | Partial cancel — short qty |
| `0x26` | Reduce Size (Long) | Partial cancel — long qty |
| `0x27` | Modify Order (Short) | Cancel+reinsert at new price |
| `0x28` | Modify Order (Long) | — |
| `0x29` | Delete Order | Full cancel |
| `0x2A` | Trade (Short) | Off-book print |
| `0x2B` | Trade (Long) | — |
| `0x2C` | Trade Break | Trade bust |
| `0x2D` | End of Session | — |

**Short vs. Long variants**: PITCH uses short/long message variants where the short form uses smaller field widths (2-byte vs. 4-byte quantity, 6-byte vs. 8-byte symbol). This is a bandwidth optimization — the majority of messages are short variants. A PITCH feed handler must support both.

### Key Differences from ITCH

| Feature | ITCH 5.0 | PITCH |
|---|---|---|
| Sequence width | 64-bit | 32-bit |
| Timestamp | Nanoseconds since midnight (8B) | Nanoseconds since midnight (8B) in some versions; absent in others |
| Order ID | 64-bit monotonic ref# | 64-bit client-style ID (can re-use) |
| Price encoding | Fixed-point (4B), implicit 4 decimal places | ASCII-numeric string (10 bytes!) in some versions |
| Symbol field | Left-padded, space-filled | Right-padded, space-filled |
| Gap recovery | MoldUDP64 retransmit server | GRP + Spin Server |
| Message variants | Single canonical | Short/Long variants |

**Price encoding note**: Some PITCH versions encode price as a 10-byte ASCII decimal string (e.g., `"0001850000"` = $185.00). This is a significant parsing cost versus ITCH's 4-byte integer, and PITCH feed handlers typically precompile a fast decimal-to-integer parser for this field.

### PITCH for Options (CBOE Options Depth)
CBOE's options market data has additional message types:
- **Auction Update / Auction Summary**: for opening rotation and FLEX auctions
- **Complex Order** messages: multi-leg strategy orders with separate leg-level fields
- **Trading Status** with options-specific halt codes

---

## 5. SBE (Simple Binary Encoding)

### Overview
SBE is an **encoding standard**, not a protocol. It defines how messages are **laid out in bytes on the wire** — it is the binary serialization layer. SBE was formally standardized by the **FIX Trading Community** and adopted by CME, NASDAQ, ICE, and others. The goal: **encode/decode at memory-bandwidth speed**, with zero heap allocation, zero branching on field presence, and direct memory-mapped access.

### Design Principles

**1. Fixed-Length Fields by Default**
Every field has a statically known offset from the message header. There are no variable-length field searches. This enables:
```c
// Access price directly at compile-time-known offset
double price = *(double*)(buf + PRICE_OFFSET);
// Or equivalently, via a generated accessor:
auto price = msg.price();  // Compiles to a single MOV instruction
```

**2. Fly-weight Pattern / Zero-Copy Decoding**
SBE-generated decoders are **flyweight wrappers** around raw byte buffers. The decoder object holds a pointer and an offset; accessing a field computes `base_ptr + field_offset`. No data is ever copied into the decoder struct:

```cpp
// SBE-generated C++ decoder (conceptual)
class NewOrderSingle {
    const char* buf_;
public:
    uint64_t clOrdId()  const { return le64toh(*(uint64_t*)(buf_ + 0));  }
    int64_t  price()    const { return le64toh(*(int64_t*) (buf_ + 8));  }
    uint32_t orderQty() const { return le32toh(*(uint32_t*)(buf_ + 16)); }
    char     side()     const { return *(buf_ + 20);                      }
};
```

**3. Schema-Driven Code Generation**
SBE schemas are defined in XML:
```xml
<sbe:message name="NewOrderSingle" id="14" blockLength="46">
  <field name="clOrdId"      id="11"  type="uint64"    offset="0"/>
  <field name="symbol"       id="55"  type="char8"     offset="8"/>
  <field name="side"         id="54"  type="SideEnum"  offset="16"/>
  <field name="orderQty"     id="38"  type="uint32"    offset="17"/>
  <field name="price"        id="44"  type="Decimal64" offset="21"/>
  <field name="timeInForce"  id="59"  type="TIFEnum"   offset="29"/>
</sbe:message>
```

Code generators (official SBE toolchain, or custom) produce **zero-overhead C++, Java, Go, or Rust accessors** from the schema. The generated code is typically a few hundred bytes of inline memory accesses — the entire decoder fits in L1 instruction cache.

**4. Composites and Enums**
SBE supports:
- **Composites**: fixed-size structs embedded inline (e.g., a `Decimal64` = `{mantissa: int64, exponent: int8}`)
- **Enums**: byte/uint16 values with named constants (no string-to-enum conversion on parse)
- **Sets/Bit flags**: packed bitfields for flags like `ExecInst`

**5. Repeating Groups**
Variable-length content (e.g., a list of legs in a multi-leg order) is handled via **repeating groups** — a counted array of fixed-length sub-blocks. The decoder walks groups sequentially, maintaining a cursor:
```
[BlockLength: 2][NumInGroup: 2][Entry1...][Entry2...]...
```
This preserves forward-only, branchless iteration — the group size is known upfront from the count, so the parser never needs to scan for delimiters.

**6. Variable-Length Data**
Free-form strings or byte blobs are appended at the end of a message after all fixed fields and groups:
```
[Length: 2][Data: N bytes]
```
These are accessed last, after fixed fields, maintaining sequential memory access patterns (cache-friendly).

### SBE vs. Other Encodings

| Feature | SBE | Protobuf | FIX ASCII | FAST |
|---|---|---|---|---|
| Decode cost | 1–3 ns | 50–200 ns | 500+ ns | 20–100 ns |
| Allocation | Zero | Yes (objects) | Yes (strings) | Zero |
| Schema required | Yes | Yes | Yes (spec) | Yes |
| Variable fields | End-of-message | Inline | Tag-value | PMAP bits |
| Endianness | Little-endian | Little-endian (varint) | N/A | Big-endian |
| Self-describing | No | No (w/ reflection: yes) | Partially | No |
| Random field access | O(1) | O(n) | O(n) | O(n) |

### SBE in Practice: CME MDP 3.0
CME's **Market Data Platform 3.0** (MDP 3.0) is the canonical production deployment of SBE at scale. CME sends **~10 million messages/second** across futures and options markets. The SBE schema for MDP 3.0 defines ~50 message types, including:
- `MDIncrementalRefreshTrade` — trade prints
- `MDIncrementalRefreshBook` — incremental order book updates (repeating group of price level changes)
- `SnapshotFullRefresh` — book snapshots for recovery
- `SecurityDefinition` — instrument reference data
- `SecurityStatus` — trading halts, phase transitions

The SBE encoding of an incremental book refresh carrying 5 price level changes fits in a **single Ethernet frame** (~400 bytes), whereas the equivalent FIX message would be 3–5× larger and require ASCII parsing.

---

## Cross-Protocol Architecture in an HFT Firm

A complete HFT stack typically layers these protocols as follows:

```
                        ┌─────────────────────────────┐
  Market Data Layer     │  ITCH 5.0 / PITCH / SBE     │  ← UDP Multicast / Kernel-bypass
                        │  (Feed Handlers per venue)  │
                        └────────────┬────────────────┘
                                     │ Normalized L2 Book
                        ┌────────────▼────────────────┐
  Signal / Alpha Layer  │   Strategy Engine           │  ← Sub-microsecond processing
                        │   (FPGA or optimized C++)   │
                        └────────────┬────────────────┘
                                     │ Order decisions
                        ┌────────────▼────────────────┐
  Order Entry Layer     │  OUCH / BOE / ETI / XETRA   │  ← TCP, kernel-bypass
                        │  (Per-venue OMS adapters)   │
                        └────────────┬────────────────┘
                                     │ Fills / Acks
                        ┌────────────▼────────────────┐
  Post-Trade Layer      │  FIX 5.0 Drop Copy          │  ← Prime broker, regulator
                        │  FIXML / SWIFT / FpML       │
                        └─────────────────────────────┘
```

---

## Summary: Engineering Tradeoffs

| Protocol | Layer | Transport | Encoding | Latency Class | HFT Role |
|---|---|---|---|---|---|
| FIX 5.0 | Order Entry / Post-trade | TCP | ASCII tag=value | Milliseconds | Drop copy, OMS, prime broker |
| OUCH | Order Entry | TCP (SoupBinTCP) | Binary structs | ~100 ns | Co-location order submission |
| ITCH 5.0 | Market Data | UDP (MoldUDP64) | Binary structs | ~50–100 ns | Full book reconstruction |
| PITCH | Market Data | UDP (GRP) | Binary structs | ~50–100 ns | Full book reconstruction (CBOE) |
| SBE | Encoding Layer | Agnostic | Binary, fixed-offset | 1–3 ns decode | CME MDP 3.0, any binary protocol |

The overarching engineering philosophy across all HFT-native protocols is identical: **eliminate parsing, eliminate allocation, eliminate indirection**. Every design decision — fixed field widths, monotonic sequence numbers, flyweight decoders, fixed-point arithmetic — exists to allow the application to spend its CPU budget on alpha generation rather than protocol overhead.



---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Provide a thorough description of the dominant protocols such as FIX 5.0, OUCH, ITCH 5.0 (NASDAQ), PITCH (CBOE/BATS), and SBE (Simple Binary Encoding) in a high-frequency trading environment. This description is intended for a computer science expert. Describe the important technical and business features of these protocols.
