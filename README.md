# Single-Producer Single-Consumer (SPSC) lock-free ring buffer


## Single-Producer Single-Consumer (SPSC)
+ [SPSC Introduction](./doc/spsc-introduction.md)
+ [SPSC Lock-Free Ring Buffer for High-Frequency Trading](./doc/spsc-thorough-description.md)
+ [Huge Pages & TLB in SPSC](./doc/spsc-tlb-and-huge-pages.md)
+ [Detailed Design Notes and Trade-Offs](./doc/spsc-design.md)
+ [Memory Ordering in SPSC Lock-Free Ring Buffers](./doc/spsc-memory-ordering.md)
+ [Atomics vs. Mutexes in SPSC](./doc/spsc-atomics-mutexes-claude.md)


## High Frequency Trading and SPSC
+ [Application in HFT](hft-queues-app-short.md)
+ [Application in HFT: In-Depth Analysis](hft-queues-app-extended.md)
+ [Importance of queue size in HFT](./doc/hft-queue-size.md)
+ [Vertical scaling in HFT](./doc/hft-scaling-vertical.md)
+ [Horizontal scaling in HFT](./doc/hft-scaling-horizontal.md)


## High Frequency Trading Pipeline and SPSC
+ [Stage 1: NIC → Market Data Handler (Kernel Bypass)](./doc/hft-pipeline-stage-1.md)
+ [Stage 2: Market Data Parser → Order Book Engine](./doc/hft-pipeline-stage-2.md)
+ [Stage 3: Order Book Engine → Alpha / Signal Engine](./doc/hft-pipeline-stage-3.md)
+ [Stage 4: Signal Engine → Order Management System (OMS)](./doc/hft-pipeline-stage-4.md)
+ [Stage 5: OMS → Exchange Gateway (Outbound)](./doc/hft-pipeline-stage-5.md)
+ [Stage 6: Exchange Gateway → OMS (Inbound Execution Reports)](./doc/hft-pipeline-stage-6.md)
+ [Stage 7: OMS → Risk Manager](./doc/hft-pipeline-stage-7.md)
+ [Stage 8: Any Thread → Logger](./doc/hft-pipeline-stage-8.md)


## Specific Data Structures in HFT
+ [Order book architecture in HFT systems](./doc/hft-order-book.md)
+ [Matching Engine Architecture for HFT](./doc/hft-matching-engine.md)
+ [Intrusive Linked List for an HFT Order Queue](./doc/hft-intrusive-linked-list.md)
+ [Flat Circular Array Order Queue for HFT](./doc/hft-flat-circular-array.md)
+ [Flat Hash Map + Sorted Intrusive Tree for an HFT Order Queue](./doc/hft-flat-hash-map.md)


## Operating Systems in HFT
+ [Kernel Bypass in HFT: DPDK & Solarflare OpenOnload](./doc/hft-kernale-bypass.md)
+ [CPU Pinning in HFT Systems](hft-cpu-pinning.md)
+ [Cache Pathologies in HFT Systems](./doc/hft-cache-pathologies.md)
+ [Rocky Linux for HFT](./doc/hft-rocky-linux.md)


## Hardware in HFT
+ [x86-64 CPUs for High-Frequency Trading](./doc/hft-cpu-and-cache.md)
+ [Setup of x86-64 Linux HFT System](./doc/spsc-x86-64-setup.md)
+ [x86-64 vs. IBM POWER on Linux](./doc/spsc-x86-64-vs-ibm-power.md)

## Compiler Optimization in HFT
+ [Performance Analysis Tools for C++ HFT Systems](./doc/hft-performace-tools.md)
+ [Compiler Optimization Techniques for C++ in HFT Systems](./doc/hft-compiler-optimization.md)


## Logging Libraries in HFT
+ [Quill Logger in HFT](./doc/hft-quill.md)
+ [NanoLog Logger in HFT](./doc/hft-nanolog.md)
+ [SPDLog Logger in HFT](./doc/hft-spdlog.md)
+ [Measuring Execution Time in HFT](./doc/hft-execution-time.md)


## Market Data Protocols in HFT
+ [High-Frequency Trading Protocols](./doc/hft-protocols.md):
  + FIX 5.0 (Financial Information eXchange)
  + OUCH (NASDAQ Order Entry Protocol)
  + ITCH 5.0 (NASDAQ Market Data Feed)
  + PITCH (CBOE/BATS Market Data Protocol)
  + SBE (Simple Binary Encoding)


## Message Queue Systems in HFT
+ [Apache Kafka in HFT Environments](./doc/hft-kafka-message.md)
+ [Kafka's Role in HFT Environments](./doc/hft-kafka-role.md)
+ [Solace PubSub+ in HFT Environments](./doc/hft-solace-message.md)
+ [IBM MQ in HFT Environments](./doc/hft-ibm-mq-message.md)
+ [Kafka vs. Solace](./doc/hft-kafka-vs-solace.md)


## My LinkedIn Posts Related to HFT
+ [Trading at light speed by David Gross, Meeting C++ 2022](https://github.com/romz-pl/posts-linkedin/blob/main/099-meetingcpp-low-latency/README.md)
+ [Lock-Free Single Producer Single Consumer FIFO, CppCon 2023](https://github.com/romz-pl/posts-linkedin/blob/main/098-cppcon-spsc-lock-free/README.md)
+ [Lock-Free Programming by Herb Sutter, CppCon 2014](https://github.com/romz-pl/posts-linkedin/blob/main/097-cppcon2014-lock-free/README.md)
+ [Microseconds matter. Ring Buffer (SPSC queue) by Erik Rigtorp](https://github.com/romz-pl/posts-linkedin/blob/main/100-spsc-by-rigtorp/README.md)
+ [Ultrafast Trading Systems by David Gross](https://github.com/romz-pl/posts-linkedin/blob/main/101-cppcon-ultrafast/README.md)


## Tests for SPSC implemenation
+ [Description of Basic Tests](./doc/test_basic.md)
+ [Description of Concurrent Tests](./doc/test_concurrent.md)


## Compare ChatGPT (OpenAI) with Claude (Anthropic)
+ [Atomics vs. Mutexes in SPSC - Claude](./doc/spsc-atomics-mutexes-claude.md)
+ [Atomics vs. Mutexes in SPSC - ChatGPT](./doc/spsc-atomics-mutexes-chatgpt.md)


---

## 📖 References
+ 🎥 "Lock-Free Programming (or, Juggling Razor Blades), Part I", [16 Oct 2014](https://www.youtube.com/watch?v=c1gO9aB9nbs)
+ 🎥 "Lock-Free Programming (or, Juggling Razor Blades), Part II", [16 Oct 2014](https://www.youtube.com/watch?v=CmxkPChOcvw)
+ 🎥 "Low-Latency Lock-Free Ring-Buffer in C - Lock Free Programming (Part #2)", [13 Mar 2024](https://www.youtube.com/watch?v=aYwmopy6cdY)
+ 🎥 "Single Producer Single Consumer Lock-free FIFO From the Ground Up", CppCon 2023, [22 Feb 2024](https://www.youtube.com/watch?v=K3P_Lmq6pw0)
+ 🎥 "SPSC Queues: From Naive to Lock-Free", [24 Jan 2026](https://www.youtube.com/watch?v=PFxzyWMoG_A)
+ 🎥 "Trading at light speed: designing low latency systems in C++", Meeting C++ 2022, [2 Jan 2023](https://www.youtube.com/watch?v=8uAW5FQtcvE)
+ 🎥 "What is Low Latency C++? (Part 1)", CppNow 2023, [18 Aug 2023](https://www.youtube.com/watch?v=EzmNeAhWqVs)
+ 🎥 "What is Low Latency C++? (Part 2)", CppNow 2023, [18 Aug 2023](https://www.youtube.com/watch?v=5uIsadq-nyk)
+ 🎥 "When Nanoseconds Matter: Ultrafast Trading Systems in C++", CppCon 2024, [28 Feb 2025](https://www.youtube.com/watch?v=sX2nF1fW7kI)
+ Erik Rigtorp, "Optimizing a Ring Buffer for Throughput", [14 April 2026](https://rigtorp.se/ringbuffer/)
+ Erik Rigtorp, [SPSCQueue GitHub repo](https://github.com/rigtorp/SPSCQueue)
+ Jackson Allan, "An Extensive Benchmark of C and C++ Hash Tables", [14 Apr 2026](https://jacksonallan.github.io/c_cpp_hash_tables_benchmark/)
+ Paul E. McKenney, "Is Parallel Programming Hard, And, If So, What Can You Do About It?", June 11, 2023, [Release v2023.06.11a](https://arxiv.org/pdf/1701.00854)
+ Peter Mbanugo, "Building a Lock-Free Single Producer, Single Consumer Queue (FIFO)", [15 Apr 2026](https://pmbanugo.me/blog/building-lock-free-spsc-queue)
+ [C++ Memory Model (cppreference)](https://en.cppreference.com/w/cpp/atomic/memory_order)
+ Boost C++ Libraries, [SPSC Lock Free Queue](https://www.boost.org/doc/libs/latest/doc/html/lockfree/reference.html)

