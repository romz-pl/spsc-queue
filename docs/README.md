# Single-Producer Single-Consumer (SPSC) lock-free ring buffer


## Single-Producer Single-Consumer (SPSC)
+ [SPSC Lock-Free Ring Buffer for High-Frequency Trading](./spsc-thorough-description.md)
+ [Detailed Design Notes and Trade-Offs](./spsc-design.md)
+ [Memory Ordering in SPSC Lock-Free Ring Buffers](./spsc-memory-ordering.md)


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





## Tests for SPSC implemenation
+ [Description of Basic Tests](./test_basic.md)
+ [Description of Concurrent Tests](./test_concurrent.md)
