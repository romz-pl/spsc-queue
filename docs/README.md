# Single-Producer Single-Consumer (SPSC) lock-free ring buffer


## Single-Producer Single-Consumer (SPSC)
+ [SPSC Lock-Free Ring Buffer for High-Frequency Trading](./spsc-thorough-description.md)
+ [Huge Pages & TLB in SPSC](./spsc-tlb-and-huge-pages.md)
+ [Setup of x86-64 Linux HFT System](./spsc-x86-64-setup.md)
+ [Detailed Design Notes and Trade-Offs](./spsc-design.md)
+ [Memory Ordering in SPSC Lock-Free Ring Buffers](./spsc-memory-ordering.md)
+ [x86-64 vs. IBM POWER on Linux](./spsc-x86-64-vs-ibm-power.md)


## High Frequency Trading and SPSC
+ [Application in HFT](hft-queues-app-short.md)
+ [Application in HFT: In-Depth Analysis](hft-queues-app-extended.md)
+ [Importance of queue size in HFT](./hft-queue-size.md)
+ [Vertical scaling in HFT](./hft-scaling-vertical.md)
+ [Horizontal scaling in HFT](./hft-scaling-horizontal.md)


## High Frequency Trading Pipeline and SPSC
+ [Stage 1: NIC → Market Data Handler (Kernel Bypass)](./hft-pipeline-stage-1.md)
+ [Stage 2: Market Data Parser → Order Book Engine](./hft-pipeline-stage-2.md)
+ [Stage 3: Order Book Engine → Alpha / Signal Engine](./hft-pipeline-stage-3.md)
+ [Stage 4: Signal Engine → Order Management System (OMS)](./hft-pipeline-stage-4.md)
+ [Stage 5: OMS → Exchange Gateway (Outbound)](./hft-pipeline-stage-5.md)
+ [Stage 6: Exchange Gateway → OMS (Inbound Execution Reports)](./hft-pipeline-stage-6.md)
+ [Stage 7: OMS → Risk Manager](./hft-pipeline-stage-7.md)
+ [Stage 8: Any Thread → Logger](./hft-pipeline-stage-8.md)


## High Frequency Trading and Specific Data Structures
+ [Order book architecture in HFT systems](./hft-order-book.md)
+ [Matching Engine Architecture for HFT](./hft-matching-engine.md)
+ [Intrusive Linked List for an HFT Order Queue](./hft-intrusive-linked-list.md)
+ [Flat Circular Array Order Queue for HFT](./hft-flat-circular-array.md)
+ [Flat Hash Map + Sorted Intrusive Tree for an HFT Order Queue](./hft-flat-hash-map.md)
+ [An Extensive Benchmark of C and C++ Hash Tables by Jackson Allan](https://jacksonallan.github.io/c_cpp_hash_tables_benchmark/)


## High Frequency Trading and Operating System
+ [Kernel Bypass in HFT: DPDK & Solarflare OpenOnload](./hft-kernale-bypass.md)
+ [CPU Pinning in HFT Systems](hft-cpu-pinning.md)
+ [Cache Pathologies in HFT Systems](./hft-cache-pathologies.md)


## High Frequency Trading and Compiler Optimization
+ [Performance Analysis Tools for C++ HFT Systems](./hft-performace-tools.md)
+ [Compiler Optimization Techniques for C++ in HFT Systems](./hft-compiler-optimization.md)


## Market Data Protocols
+ [High-Frequency Trading Protocols](./hft-protocols.md)


## Message Queue Systems in HFT
+ [Apache Kafka in HFT Environments](./hft-kafka-message.md)
+ [Kafka's Role in HFT Environments](./hft-kafka-role.md)
+ [Solace PubSub+ in HFT Environments](./hft-solace-message.md)
+ [IBM MQ in HFT Environments](./hft-ibm-mq-message.md)
+ [Kafka vs. Solace](./hft-kafka-vs-solace.md)


## Tests for SPSC implemenation
+ [Description of Basic Tests](./test_basic.md)
+ [Description of Concurrent Tests](./test_concurrent.md)



## References
+ 🎥 "Low-Latency Lock-Free Ring-Buffer in C - Lock Free Programming (Part #2)", [13 Mar 2024](https://www.youtube.com/watch?v=aYwmopy6cdY)
+ 🎥 "Single Producer Single Consumer Lock-free FIFO From the Ground Up", CppCon 2023, [22 Feb 2024](https://www.youtube.com/watch?v=K3P_Lmq6pw0)
+ 🎥 "SPSC Queues: From Naive to Lock-Free", [24 Jan 2026](https://www.youtube.com/watch?v=PFxzyWMoG_A)
+ 🎥 "Trading at light speed: designing low latency systems in C++", Meeting C++ 2022, [2 Jan 2023](https://www.youtube.com/watch?v=8uAW5FQtcvE)
+ 🎥 "What is Low Latency C++? (Part 1)", CppNow 2023, [18 Aug 2023](https://www.youtube.com/watch?v=EzmNeAhWqVs)
+ 🎥 "What is Low Latency C++? (Part 2)", CppNow 2023, [18 Aug 2023](https://www.youtube.com/watch?v=5uIsadq-nyk)
+ 🎥 "When Nanoseconds Matter: Ultrafast Trading Systems in C++", CppCon 2024, [28 Feb 2025](https://www.youtube.com/watch?v=sX2nF1fW7kI)
+ Erik Rigtorp, "Optimizing a Ring Buffer for Throughput", [14 April 2026](https://rigtorp.se/ringbuffer/)
+ Erik Rigtorp, [SPSCQueue GitHub repo](https://github.com/rigtorp/SPSCQueue)
+ Paul E. McKenney, "Is Parallel Programming Hard, And, If So, What Can You Do About It?", June 11, 2023, [Release v2023.06.11a](https://arxiv.org/pdf/1701.00854)

