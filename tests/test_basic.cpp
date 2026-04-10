#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <string>
#include <vector>

#include "../include/spsc_queue.hpp"

// ── Helpers ───────────────────────────────────────────────────────────────────
// Effective capacity of a queue with buffer size N is N-1 (one slot is wasted
// to distinguish full from empty).
template<typename Q>
static bool queue_is_empty(Q& q) {
    int dummy;
    return !q.pop(dummy);   // non-destructive only when queue really is empty
}

// ── 1. Static assertions ──────────────────────────────────────────────────────
// (Verified at compile time; the test just documents the requirement.)
TEST(SPSCQueue, StaticCapacityRequirement) {
    // SPSCQueue<int, 1> q;  // would fail to compile – good
    SPSCQueue<int, 2> q2;   // smallest legal size
    (void)q2;
    SUCCEED();
}

// ── 2. Empty queue behaviour ──────────────────────────────────────────────────
TEST(SPSCQueue, PopOnEmptyReturnsFalse) {
    SPSCQueue<int, 4> q;
    int val = 42;
    EXPECT_FALSE(q.pop(val));
    EXPECT_EQ(val, 42); // output parameter must not be modified
}

TEST(SPSCQueue, InitialStateIsEmpty) {
    SPSCQueue<int, 8> q;
    int v;
    EXPECT_FALSE(q.pop(v));
}

// ── 3. Basic push / pop round-trip ────────────────────────────────────────────
TEST(SPSCQueue, SinglePushPopRoundTrip) {
    SPSCQueue<int, 4> q;
    EXPECT_TRUE(q.push(7));
    int out = 0;
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out, 7);
}

TEST(SPSCQueue, FIFOOrdering) {
    SPSCQueue<int, 8> q;
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(q.push(i));
    for (int i = 0; i < 5; ++i) {
        int out;
        ASSERT_TRUE(q.pop(out));
        EXPECT_EQ(out, i);
    }
}

// ── 4. Capacity / full-queue behaviour ────────────────────────────────────────
// Buffer size N  →  usable capacity N-1
TEST(SPSCQueue, CapacityIsNMinusOne) {
    SPSCQueue<int, 4> q; // usable capacity = 3
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.push(3));
    EXPECT_FALSE(q.push(4)); // 4th push must fail
}

TEST(SPSCQueue, PushOnFullReturnsFalse) {
    SPSCQueue<int, 2> q; // usable capacity = 1
    EXPECT_TRUE(q.push(99));
    EXPECT_FALSE(q.push(100));
}

TEST(SPSCQueue, PushFailsWhenFullThenSucceedsAfterPop) {
    SPSCQueue<int, 3> q; // capacity 2
    ASSERT_TRUE(q.push(10));
    ASSERT_TRUE(q.push(20));
    ASSERT_FALSE(q.push(30)); // full

    int v;
    ASSERT_TRUE(q.pop(v));
    EXPECT_EQ(v, 10);

    EXPECT_TRUE(q.push(30)); // now there's room
}

// ── 5. Drain to empty then refill ─────────────────────────────────────────────
TEST(SPSCQueue, DrainAndRefill) {
    SPSCQueue<int, 4> q;
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(q.push(i));
    for (int i = 0; i < 3; ++i) { int v; ASSERT_TRUE(q.pop(v)); }

    // Queue is empty again
    int v;
    EXPECT_FALSE(q.pop(v));

    // Can be refilled
    for (int i = 10; i < 13; ++i) ASSERT_TRUE(q.push(i));
    for (int i = 10; i < 13; ++i) {
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);
    }
}

// ── 6. Wrap-around (index modulo N) ───────────────────────────────────────────
// Push / pop more items than N to exercise the modulo wrap path.
TEST(SPSCQueue, IndexWrapAround) {
    SPSCQueue<int, 4> q; // N=4, capacity=3
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 3; ++i) ASSERT_TRUE(q.push(round * 10 + i));
        for (int i = 0; i < 3; ++i) {
            int v;
            ASSERT_TRUE(q.pop(v));
            EXPECT_EQ(v, round * 10 + i);
        }
    }
}

// ── 7. Non-trivial value type ─────────────────────────────────────────────────
TEST(SPSCQueue, WorksWithStrings) {
    SPSCQueue<std::string, 4> q;
    ASSERT_TRUE(q.push("hello"));
    ASSERT_TRUE(q.push("world"));
    std::string s;
    ASSERT_TRUE(q.pop(s)); EXPECT_EQ(s, "hello");
    ASSERT_TRUE(q.pop(s)); EXPECT_EQ(s, "world");
    EXPECT_FALSE(q.pop(s));
}

struct Point { int x, y; };

TEST(SPSCQueue, WorksWithStructs) {
    SPSCQueue<Point, 4> q;
    ASSERT_TRUE(q.push({3, 4}));
    Point p{};
    ASSERT_TRUE(q.pop(p));
    EXPECT_EQ(p.x, 3);
    EXPECT_EQ(p.y, 4);
}

// ── 8. pop_blocking / push_blocking (single-threaded, already-satisfied) ──────
TEST(SPSCQueue, PopBlockingReturnsImmediatelyWhenItemPresent) {
    SPSCQueue<int, 4> q;
    ASSERT_TRUE(q.push(55));
    int v = q.pop_blocking();
    EXPECT_EQ(v, 55);
}

TEST(SPSCQueue, PushBlockingSucceedsImmediatelyWhenNotFull) {
    SPSCQueue<int, 4> q;
    q.push_blocking(1);
    q.push_blocking(2);
    int v;
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 1);
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 2);
}

// ── 9. Interleaved push/pop sequence ─────────────────────────────────────────
TEST(SPSCQueue, InterleavedPushPop) {
    SPSCQueue<int, 4> q;
    int out;
    ASSERT_TRUE(q.push(1));
    ASSERT_TRUE(q.pop(out));  EXPECT_EQ(out, 1);
    ASSERT_TRUE(q.push(2));
    ASSERT_TRUE(q.push(3));
    ASSERT_TRUE(q.pop(out));  EXPECT_EQ(out, 2);
    ASSERT_TRUE(q.push(4));
    ASSERT_TRUE(q.pop(out));  EXPECT_EQ(out, 3);
    ASSERT_TRUE(q.pop(out));  EXPECT_EQ(out, 4);
    EXPECT_FALSE(q.pop(out));
}

// ── 10. Large capacity ────────────────────────────────────────────────────────
TEST(SPSCQueue, LargeCapacityFillAndDrain) {
    constexpr size_t N = 1025;
    SPSCQueue<int, N> q;
    for (int i = 0; i < static_cast<int>(N - 1); ++i) ASSERT_TRUE(q.push(i));
    EXPECT_FALSE(q.push(-1)); // full
    for (int i = 0; i < static_cast<int>(N - 1); ++i) {
        int v;
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);
    }
    int v;
    EXPECT_FALSE(q.pop(v)); // empty again
}

// ── 11. Pop does not change value on failure ──────────────────────────────────
TEST(SPSCQueue, PopDoesNotClobberValueOnFailure) {
    SPSCQueue<int, 4> q;
    int sentinel = 1234;
    EXPECT_FALSE(q.pop(sentinel));
    EXPECT_EQ(sentinel, 1234);
}

// ── main ──────────────────────────────────────────────────────────────────────
//int main(int argc, char** argv) {
//    ::testing::InitGoogleTest(&argc, argv);
//    return RUN_ALL_TESTS();
//}
