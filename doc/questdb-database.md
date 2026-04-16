# QuestDB for High-Frequency Trading Systems

---

## 1. Architectural Overview

QuestDB is a time-series database purpose-built for high-throughput ingestion and low-latency analytical queries. In a high-frequency trading (HFT) context, it operates as the **persistence and analytics layer** beneath the ultra-low-latency execution core, handling tick data, order book snapshots, trade records, and performance telemetry.

A canonical HFT stack using QuestDB looks like this:

```
┌─────────────────────────────────────────────────────────────────┐
│                      Market Data Feed                           │
│           (FIX, FAST/SBE, ITCH, proprietary UDP)                │
└───────────────────────────┬─────────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────────┐
│                    C++ Order Management System                  │
│         (kernel-bypass networking, lock-free queues,            │
│          FPGA/DPDK, CPU pinning, huge pages)                    │
│                                                                 │
│   ┌─────────────┐   ┌──────────────┐   ┌──────────────────┐     │
│   │  Alpha/     │   │  Risk Engine │   │  Execution Mgmt  │     │
│   │  Signal Gen │   │  (real-time) │   │  (smart order    │     │
│   └──────┬──────┘   └──────┬───────┘   │   routing)       │     │
│          │                 │           └────────┬─────────┘     │
└──────────┼─────────────────┼────────────────────┼───────────────┘
           │                 │                    │
┌──────────▼─────────────────▼────────────────────▼───────────────┐
│                  Async Write Buffer (LMAX Disruptor-style)      │
│            Lock-free ring buffer decouples hot path             │
└────────────────────────────┬────────────────────────────────────┘
                             │  ILP over TCP/UDP
┌────────────────────────────▼────────────────────────────────────┐
│                         QuestDB                                 │
│   InfluxDB Line Protocol Ingestion │ PostgreSQL Wire Protocol   │
│   Column-oriented storage (WAL)    │ SIMD-accelerated queries   │
│   Apache Parquet export            │ SQL + time-series ext.     │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. Ingestion Pipeline: C++ → QuestDB

### 2.1 InfluxDB Line Protocol (ILP) over TCP/UDP

QuestDB's primary high-throughput ingestion path is the **InfluxDB Line Protocol**, which is a compact, text-based wire format. From C++, you bypass QuestDB's REST API entirely and write directly to its ILP socket listener (default port 9009).

**Wire format anatomy:**
```
<table_name>[,<tag_key>=<tag_value>...] <field_key>=<field_value>[,...] [<unix_nanosecond_timestamp>]\n
```

**Example — raw tick data:**
```
trades,venue=NYSE,symbol=AAPL price=189.42,size=500i,side="B" 1713250800123456789
orderbook,symbol=MSFT,level=1 bid=415.10,ask=415.12,bid_sz=1000i,ask_sz=800i 1713250800124000000
```

### 2.2 C++ Client Implementation

QuestDB provides a native C++ client library (`questdb-client`) based on the official C client. Here is a production-grade integration pattern:

```cpp
#include <questdb/ilp/line_sender.hpp>
#include <questdb/ilp/line_sender.h>
#include <atomic>
#include <thread>
#include <concurrentqueue/concurrentqueue.h>  // moodycamel

// ----- Tick record POD (cache-line aligned) -----
struct alignas(64) TickRecord {
    int64_t  timestamp_ns;
    double   price;
    int32_t  qty;
    char     symbol[8];
    char     venue[4];
    char     side;   // 'B' or 'A'
};

// ----- QuestDB Writer (dedicated thread, not on hot path) -----
class QuestDBWriter {
public:
    explicit QuestDBWriter(const std::string& host, uint16_t port)
        : running_(true)
    {
        // Build sender using the fluent builder API
        sender_ = questdb::ilp::line_sender::from_conf(
            ("tcp::addr=" + host + ":" + std::to_string(port) + ";").c_str()
        );

        writer_thread_ = std::thread([this] { drain_loop(); });
    }

    ~QuestDBWriter() {
        running_.store(false, std::memory_order_release);
        writer_thread_.join();
    }

    // Called from hot path — wait-free enqueue
    inline bool enqueue(const TickRecord& rec) noexcept {
        return queue_.try_enqueue(rec);
    }

private:
    void drain_loop() {
        // Pin writer thread to isolated core
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(3, &cpuset);  // dedicated core
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

        TickRecord rec;
        uint64_t   batch_count = 0;

        while (running_.load(std::memory_order_acquire) || queue_.size_approx() > 0) {
            while (queue_.try_dequeue(rec)) {
                // Build ILP row
                sender_.table("trades")
                    .symbol("symbol", questdb::ilp::utf8_view{rec.symbol})
                    .symbol("venue",  questdb::ilp::utf8_view{rec.venue})
                    .column("price",  rec.price)
                    .column("qty",    static_cast<int64_t>(rec.qty))
                    .column("side",   questdb::ilp::utf8_view{&rec.side, 1})
                    .at(questdb::ilp::timestamp_nanos{rec.timestamp_ns});

                ++batch_count;

                // Flush every 4096 rows or on queue drain
                if (batch_count % 4096 == 0) {
                    sender_.flush();
                    batch_count = 0;
                }
            }

            if (batch_count > 0) {
                sender_.flush();
                batch_count = 0;
            }

            // Brief pause to avoid spinning on empty queue
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        sender_.close();
    }

    moodycamel::ConcurrentQueue<TickRecord> queue_{1 << 20};  // 1M slot ring
    questdb::ilp::line_sender              sender_;
    std::thread                             writer_thread_;
    std::atomic<bool>                       running_;
};
```

### 2.3 Nanosecond Timestamp Sourcing

HFT requires hardware-timestamping, not `std::chrono`. The typical approach:

```cpp
// Using RDTSC calibrated against PTP/IEEE-1588 hardware clock
inline int64_t hft_timestamp_ns() noexcept {
    uint64_t tsc;
    __asm__ volatile("rdtsc\n\t"
                     "shl $32, %%rdx\n\t"
                     "or %%rdx, %0"
                     : "=a"(tsc) : : "rdx");
    // Apply TSC-to-ns calibration factor (computed at startup vs PTP clock)
    return static_cast<int64_t>(tsc * tsc_to_ns_factor_ + epoch_offset_ns_);
}
```

QuestDB stores and queries at nanosecond precision — this is a direct architectural fit.

---

## 3. Storage Engine Internals Relevant to HFT

### 3.1 Column-Oriented Storage

QuestDB stores each column in a separate memory-mapped file (`*.d` data files). For HFT tick tables, this means:

- A query like `SELECT avg(price) WHERE symbol='AAPL'` only touches the `price` column file — no row deserialization overhead.
- SIMD (AVX2/AVX-512) vectorized operations scan column data at memory bandwidth limits (~50 GB/s on modern CPUs).

### 3.2 Write-Ahead Log (WAL) and Out-of-Order Ingestion

QuestDB 6.5+ introduced **WAL tables** with out-of-order (O3) data support. This is critical in HFT where:

- Feed handlers on different cores receive and timestamp data non-deterministically.
- Network jitter from co-located but non-identical paths causes minor reordering.

WAL tables buffer incoming data, sort by designated timestamp, and merge into the immutable partitioned storage. The O3 merge uses a k-way merge sort with pre-allocated merge buffers, achieving sub-millisecond commit latency at tens of millions of rows/second.

### 3.3 Partitioning

Tables partition by time (DAY, HOUR, MONTH). For HFT:
- `PARTITION BY HOUR` is typical — each partition maps to roughly 100M–500M rows at tick granularity.
- Partitions are individually memory-mappable, enabling cold partition eviction from RAM while hot partitions stay mapped.
- **Partition detach/attach** allows archiving historical partitions to Parquet/S3 without downtime.

---

## 4. Query Layer: PostgreSQL Wire Protocol

Post-trade analytics, signal backtesting, and risk dashboards query QuestDB via its PostgreSQL wire protocol on port 8812. From C++, use `libpq` directly:

```cpp
#include <libpq-fe.h>

// TWAP calculation for a symbol over last 30 seconds
const char* twap_query = R"SQL(
    SELECT
        symbol,
        sum(price * qty) / sum(qty)  AS twap,
        sum(qty)                      AS total_volume,
        count()                       AS tick_count
    FROM trades
    WHERE
        symbol = $1
        AND timestamp > dateadd('s', -30, now())
    SAMPLE BY 1s FILL(PREV)
)SQL";

PGconn* conn = PQconnectdb("host=localhost port=8812 dbname=qdb");
const char* params[1] = {"AAPL"};
PGresult* res = PQexecParams(conn, twap_query, 1, nullptr, params, nullptr, nullptr, 0);

for (int row = 0; row < PQntuples(res); ++row) {
    double twap   = atof(PQgetvalue(res, row, 1));
    long   volume = atol(PQgetvalue(res, row, 2));
    // Feed into risk model or reporting layer
}
PQclear(res);
```

### Key Time-Series SQL Extensions

| Extension | HFT Use Case |
|---|---|
| `SAMPLE BY 1s FILL(PREV)` | Resample irregular ticks to fixed bars |
| `LATEST ON timestamp PARTITION BY symbol` | Retrieve latest BBO per symbol — O(1) lookup |
| `ASOF JOIN` | Join two tick streams on nearest timestamp |
| `LT JOIN` | Join on strictly less-than timestamp |
| `dateadd()`, `datediff()` | Sliding window aggregations |

The `LATEST ON ... PARTITION BY symbol` construct is particularly powerful — it implements a per-symbol last-value index that QuestDB maintains explicitly, making current order book retrieval constant time regardless of table size.

---

## 5. Critical System Design Considerations

### 5.1 Hot Path Isolation

QuestDB **must never be on the hot execution path**. The write is always asynchronous:

```
Market event → C++ strategy (nanoseconds) → lock-free queue → QuestDB writer thread (microseconds)
```

Typical hot-path latency budget in HFT: **200 ns – 2 µs**. A QuestDB ILP TCP write, even with kernel bypass, adds **2–10 µs** — acceptable only off-path.

### 5.2 Memory Configuration

```bash
# /etc/questdb/server.conf

# Commit lag for O3 WAL merge window (microseconds)
cairo.o3.max.lag=500000

# Writer memory for column appends
cairo.writer.data.append.page.size=16777216

# Shared worker count for parallel column writes
shared.worker.count=8

# ILP receiver thread count
line.tcp.io.worker.count=4

# Huge pages for column data mapping
cairo.sql.sort.key.page.size=33554432
```

### 5.3 Network Topology

In a co-location environment:

```
HFT Server ──10GbE/RDMA──▶ QuestDB Server (separate physical host)
```

Or on the same host with loopback ILP using `SO_REUSEPORT` and CPU affinity to prevent QuestDB's I/O threads from contending with strategy threads.

---

## 6. Pros and Cons Evaluation

### ✅ Pros

**1. Ingestion Throughput**
QuestDB's ILP ingestion benchmarks at **4–8 million rows/second** on commodity hardware — sufficient for consolidated tape ingestion across all US equity symbols simultaneously. Its commit log design avoids locking during writes.

**2. Nanosecond Timestamp Native Support**
Unlike InfluxDB or TimescaleDB, QuestDB stores timestamps internally as `int64_t` nanoseconds since Unix epoch. No precision loss, no conversion overhead, and direct compatibility with hardware PTP clocks and TSC-derived timestamps.

**3. SIMD-Accelerated Query Engine**
AVX2 vectorization on aggregations, filters, and joins allows analytical queries over billions of rows in seconds. This matters for real-time signal validation, intraday P&L attribution, and risk exposure calculations.

**4. `LATEST ON` Index — O(1) Symbol Lookup**
The dedicated last-row-per-partition index makes "give me the current mid-price for all 8,000 symbols" a microsecond-scale operation, not a full table scan.

**5. `ASOF JOIN` and `SAMPLE BY` for Signal Research**
These are first-class SQL constructs that eliminate the need for Pandas/Python for signal research — joins between tick streams are expressed in a single query rather than complex application logic.

**6. Zero-Dependency Deployment**
QuestDB ships as a single JVM process (~50 MB JAR) or Docker image. No Zookeeper, no Kafka dependency for basic operation, no cluster management overhead for a single-node deployment. Operationally simpler than InfluxDB OSS or TimescaleDB.

**7. ILP Compatibility with Existing Tooling**
Teams migrating from InfluxDB can reuse existing ILP client code with minimal changes. Grafana, Telegraph, and most InfluxDB-compatible dashboards work out of the box.

**8. Apache Parquet Export**
Historical partitions can be exported to Parquet for archival to object storage (S3, GCS) or consumption by Spark/Arrow-based backtesting frameworks — clean separation of hot operational data and cold research data.

**9. Open Source with Commercial Enterprise Option**
The core engine is Apache 2.0 licensed. No licensing cost for teams running their own infrastructure, which matters in a domain where infrastructure margins are closely managed.

---

### ❌ Cons

**1. JVM Overhead and GC Pauses**
QuestDB runs on the JVM. Although it uses off-heap memory aggressively (via `sun.misc.Unsafe` and memory-mapped files) to minimize GC pressure, GC pauses from the JVM itself remain a risk. In worst-case scenarios, a GC pause can delay ILP acknowledgment by tens of milliseconds — unacceptable if the system is foolishly placed on the critical path. Mitigation requires careful JVM tuning (`-XX:+UseZGC` or Shenandoah GC, `-Xmx` sizing, off-heap allocation maximization).

**2. Not a Tick Database / No Built-in Order Book Reconstruction**
QuestDB is a general-purpose time-series store, not an HFT-specialized tick database like KDB+/q. It has no built-in primitives for order book reconstruction, level-2 to level-3 data modeling, or FIX message parsing. These must be implemented at the application layer.

**3. No Clustering / Horizontal Scalability (OSS)**
The open-source version is single-node only. There is no built-in replication, sharding, or distributed query execution. For an HFT firm ingesting the full CME or NSE feed (potentially 50M+ events/second at market open), a single QuestDB node becomes the bottleneck. High-availability requires custom replication logic or the commercial Enterprise tier.

**4. Limited Ecosystem vs. KDB+**
KDB+/q is the industry standard for HFT data storage, and for good reason: its vector-processing `q` language, in-memory table model, and decades of HFT-specific optimizations are unmatched. QuestDB lacks equivalent native support for trade-specific analytics like VWAP curves with volume attribution, complex event processing, or streaming CEP queries. The QuestDB SQL dialect, while expressive, is not as HFT-domain-specific as q.

**5. WAL Commit Latency Under Write Pressure**
At extremely high ingest rates, WAL O3 merges can introduce **commit lag** (configurable, defaults to 500ms). This means data written 500ms ago may not yet be queryable. For strategies that need to query their own recent writes (e.g., self-monitoring for fill rate), this introduces a blind window.

**6. No Native Streaming / CEP**
QuestDB is fundamentally a batch-query engine — it has no native publish/subscribe, streaming query, or complex event processing capability. Real-time alerting (e.g., "alert when spread exceeds 5 bps for more than 100ms") requires polling or an external CEP layer (Apache Flink, Chronicle, etc.).

**7. ILP Write Acknowledgment Semantics**
By default, ILP over TCP is fire-and-forget with no per-row acknowledgment. Write failures (network drops, backpressure) surface only as connection resets, requiring the application layer to implement retry/deduplication logic. This is non-trivial in a production HFT environment where data integrity is a regulatory requirement.

**8. Schema Evolution Constraints**
Columns can be added to QuestDB tables but not removed or renamed without table recreation. Column type changes are also unsupported. In a fast-moving HFT R&D environment where data schemas evolve frequently (adding new signal features, changing tag cardinality), this creates operational friction.

**9. Memory-Mapped File Limits**
QuestDB's column storage relies heavily on `mmap`. On Linux, this is constrained by `vm.max_map_count`. Tables with high cardinality tag columns (e.g., per-order-ID as a symbol column) generate enormous numbers of partition files and can exhaust system mmap limits, requiring careful schema design to avoid anti-patterns.

---

## 7. Verdict: When to Use QuestDB in HFT

| Scenario | Recommendation |
|---|---|
| **Tick data archival & post-trade analytics** | ✅ Excellent fit |
| **Real-time signal backtesting (intraday)** | ✅ Strong fit |
| **Risk exposure dashboards** | ✅ Strong fit |
| **Primary execution-path data store** | ❌ Never — too much latency |
| **Ultra-low-latency order book store** | ❌ Use Redis/Chronicle Map |
| **Full exchange feed capture (50M+ e/s)** | ⚠️ Only with Enterprise clustering |
| **Replacing KDB+ in a mature HFT firm** | ⚠️ Significant capability gap remains |

QuestDB occupies a compelling niche as a **high-throughput, low-cost, SQL-accessible tick store** for HFT firms that cannot justify KDB+ licensing costs or operational complexity, while needing substantially better time-series performance than general-purpose databases like PostgreSQL/TimescaleDB. It is best understood as the **analytics and persistence tier** of an HFT system, not a replacement for in-process ultra-low-latency data structures.

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Provide a detailed explanation of the application of database QuestDB for deploying a high-frequency trading system implemented in C++. This description is intended for a computer science expert. Next, evaluate the pros and cons of using database QuestDB for a high-frequency trading system.
