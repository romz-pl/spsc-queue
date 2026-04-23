#include <array>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "../include/mpmc.h"

struct MarketEvent {
    std::uint64_t  timestamp_ns;
    std::uint32_t  instrument_id;
    double         price;
    std::uint64_t  quantity;
};

int main() {
    using namespace mpmc;

    // 1 << 14 = 16384 slots.  In a real HFT system you would size this
    // based on the peak burst rate × maximum acceptable latency budget.
    MPMCRingBuffer<MarketEvent, 1 << 14> queue;

    constexpr int kEvents   = 2'000'000;
    constexpr int kProducers = 2;
    constexpr int kConsumers = 2;

    std::atomic<std::int64_t> total_consumed{0};
    std::vector<std::thread> threads;
    threads.reserve(kProducers + kConsumers);

    auto t0 = std::chrono::steady_clock::now();

    // Producers
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            for (int i = p; i < kEvents; i += kProducers) {
                queue.push(MarketEvent{
                    .timestamp_ns  = static_cast<std::uint64_t>(i),
                    .instrument_id = static_cast<std::uint32_t>(p),
                    .price         = 100.0 + i * 0.001,
                    .quantity      = static_cast<std::uint64_t>(i % 1000 + 1)
                });
            }
        });
    }

    // Consumers
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            std::int64_t local = 0;
            MarketEvent ev{};
            while (total_consumed.load(std::memory_order_relaxed) < kEvents) {
                if (queue.try_pop(ev)) {
                    ++local;
                    total_consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    detail::cpu_pause();
                }
            }
            (void)local;
        });
    }

    for (auto& t : threads) t.join();

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("Throughput: %.2f M events/sec  (%.2f ns/event)\n",
                kEvents / secs / 1e6,
                secs * 1e9 / kEvents);
    std::printf("Queue size at exit: %zu\n", queue.size());
    return 0;
}
