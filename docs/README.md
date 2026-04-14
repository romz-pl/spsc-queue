# Single-Producer Single-Consumer (SPSC) lock-free ring buffer


## Single-Producer Single-Consumer (SPSC)
+ [SPSC Lock-Free Ring Buffer for High-Frequency Trading](./spsc-thorough-description.md)
+ [Huge Pages & TLB in SPSC](./spsc-tlb-and-huge-pages.md)
+ [Setup of x86-64 Linux HFT System](./spsc-x86-64-setup.md)
+ [Detailed Design Notes and Trade-Offs](./spsc-design.md)
+ [Memory Ordering in SPSC Lock-Free Ring Buffers](./spsc-memory-ordering.md)
+ [x86-64 vs. IBM POWER on Linux](./spsc-x86-64-vs-ibm-power.md)


## High Frequency Trading (HFT) and SPSC
+ [Application in HFT](hft-queues-app-short.md)
+ [Application in HFT: In-Depth Analysis](hft-queues-app-extended.md)
+ [Importance of queue size in HFT](./hft-queue-size.md)
+ [Vertical scaling in HFT](./hft-scaling-vertical.md)
+ [Horizontal scaling in HFT](./hft-scaling-horizontal.md)
+ [Order book architecture in HFT systems](./hft-order-book.md)
+ [Kernel Bypass in HFT: DPDK & Solarflare OpenOnload](./hft-kernale-bypass.md)
+ [CPU Pinning in HFT Systems](hft-cpu-pinning.md)
+ [Cache Pathologies in HFT Systems](./hft-cache-pathologies.md)
+ [Performance Analysis Tools for C++ HFT Systems](./hft-performace-tools.md)
+ [Compiler Optimization Techniques for C++ in HFT Systems](./hft-compiler-optimization.md)



## High Frequency Trading Pipeline and SPSC
+ [Stage 1: NIC → Market Data Handler (Kernel Bypass)](./hft-pipeline-stage-1.md)
+ [Stage 2: Market Data Parser → Order Book Engine](./hft-pipeline-stage-2.md)
+ [Stage 3: Order Book Engine → Alpha / Signal Engine](./hft-pipeline-stage-3.md)
+ [Stage 4: Signal Engine → Order Management System (OMS)](./hft-pipeline-stage-4.md)
+ [Stage 5: OMS → Exchange Gateway (Outbound)](./hft-pipeline-stage-5.md)
+ [Stage 6: Exchange Gateway → OMS (Inbound Execution Reports)](./hft-pipeline-stage-6.md)
+ [Stage 7: OMS → Risk Manager](./hft-pipeline-stage-7.md)
+ [Stage 8: Any Thread → Logger](./hft-pipeline-stage-8.md)


## Market Data
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

+ ## References
+ 🎥 Charles Frasch, "Single Producer Single Consumer Lock-free FIFO From the Ground Up", CppCon 2023, [22 Feb 2024](https://www.youtube.com/watch?v=K3P_Lmq6pw0)
+ 🎥 David Gross, "When Nanoseconds Matter: Ultrafast Trading Systems in C++", CppCon 2024, [28 Feb 2025](https://www.youtube.com/watch?v=sX2nF1fW7kI)
+ 🎥 David Gross, "Trading at light speed: designing low latency systems in C++", Meeting C++ 2022, [2 Jan 2023](https://www.youtube.com/watch?v=8uAW5FQtcvE)
+ Erik Rigtorp, "Optimizing a Ring Buffer for Throughput", [14 April 2026](https://rigtorp.se/ringbuffer/)
+ Erik Rigtorp, [SPSCQueue GitHub repo](https://github.com/rigtorp/SPSCQueue)

