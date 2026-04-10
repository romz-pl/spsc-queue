#include "../include/spsc_queue.hpp"
#include <iostream>

// ── Example: producer/consumer over 1 000 000 integers ────────────────────────

static constexpr size_t QUEUE_CAP  = 1024;      // slots (power-of-2 recommended)
static constexpr int    ITEM_COUNT = 1'000'000;
static constexpr int    SENTINEL   = -1;         // signals "no more items"

int main() {
    SPSCQueue<int, QUEUE_CAP> q;

    // ── Producer ──────────────────────────────────────────────────────────────
    std::thread producer([&] {
        for (int i = 0; i < ITEM_COUNT; ++i)
            q.push_blocking(i);

        q.push_blocking(SENTINEL);   // tell the consumer we're done
        std::cout << "[producer] sent " << ITEM_COUNT << " items + sentinel\n";
    });

    // ── Consumer ──────────────────────────────────────────────────────────────
    std::thread consumer([&] {
        long long sum      = 0;
        int       received = 0;

        while (true) {
            int val = q.pop_blocking();
            if (val == SENTINEL) break;

            sum += val;
            ++received;
        }

        // Expected sum: 0+1+…+(N-1) = N*(N-1)/2
        const long long expected = static_cast<long long>(ITEM_COUNT) *
                                   (ITEM_COUNT - 1) / 2;

        std::cout << "[consumer] received " << received << " items\n";
        std::cout << "[consumer] sum      = " << sum      << "\n";
        std::cout << "[consumer] expected = " << expected << "\n";
        assert(sum == expected && "data race or lost item detected!");
        std::cout << "[consumer] assertion passed ✓\n";
    });

    producer.join();
    consumer.join();

    // ── Quick non-blocking API demo ───────────────────────────────────────────
    std::cout << "\n── non-blocking demo ──\n";
    SPSCQueue<std::string, 4> sq;   // tiny queue so we can fill it easily

    // Fill until full
    int pushed = 0;
    while (sq.push("msg-" + std::to_string(pushed))) ++pushed;
    std::cout << "pushed " << pushed << " strings before queue was full\n";

    // Try one more — must fail
    bool ok = sq.push("overflow");
    assert(!ok && "queue should be full");

    // Drain
    std::string s;
    int popped = 0;
    while (sq.pop(s)) {
        std::cout << "  popped: " << s << "\n";
        ++popped;
    }
    assert(popped == pushed);
    std::cout << "non-blocking demo passed ✓\n";
}

