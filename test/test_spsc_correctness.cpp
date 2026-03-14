#include <gtest/gtest.h>
#include "spsc_queue.hpp"

#include <thread>
#include <vector>
#include <numeric>
#include <cstdint>

// ============================================================================
// Single-threaded correctness tests
// ============================================================================

TEST(SPSCSingleThreaded, PushAndPopFIFO) {
    SPSCQueue<int, 8> q;
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.push(3));

    int v;
    EXPECT_TRUE(q.pop(v)); EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.pop(v)); EXPECT_EQ(v, 2);
    EXPECT_TRUE(q.pop(v)); EXPECT_EQ(v, 3);
}

TEST(SPSCSingleThreaded, PopFromEmpty) {
    SPSCQueue<int, 8> q;
    int v = -1;
    EXPECT_FALSE(q.pop(v));
    EXPECT_EQ(v, -1); // untouched
}

TEST(SPSCSingleThreaded, PushToFull) {
    SPSCQueue<int, 4> q;
    EXPECT_TRUE(q.push(10));
    EXPECT_TRUE(q.push(20));
    EXPECT_TRUE(q.push(30));
    EXPECT_TRUE(q.push(40));
    EXPECT_FALSE(q.push(50)); // queue full — capacity is 4
}

TEST(SPSCSingleThreaded, FullThenDrainFIFO) {
    SPSCQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(q.push(i));

    int v;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.pop(v));
}

TEST(SPSCSingleThreaded, WrapAround) {
    // Fill, drain half, fill again — exercises index wrap-around
    SPSCQueue<int, 8> q;

    for (int i = 0; i < 8; ++i)  ASSERT_TRUE(q.push(i));

    int v;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);
    }

    for (int i = 8; i < 12; ++i) ASSERT_TRUE(q.push(i));

    // Drain remaining 8 items in FIFO order: [4..7, 8..11]
    std::vector<int> expected = {4, 5, 6, 7, 8, 9, 10, 11};
    for (int exp : expected) {
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, exp);
    }
    EXPECT_FALSE(q.pop(v));
}

TEST(SPSCSingleThreaded, WrapAroundExtended) {
    // Push Capacity+1 items in two batches, verifying FIFO across the wrap
    SPSCQueue<uint64_t, 16> q;

    // First batch: fill
    for (uint64_t i = 0; i < 16; ++i) ASSERT_TRUE(q.push(i));
    EXPECT_FALSE(q.push(999)); // full

    // Drain half
    uint64_t v;
    for (uint64_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);
    }

    // Second batch: 8 more (indices wrap around)
    for (uint64_t i = 16; i < 24; ++i) ASSERT_TRUE(q.push(i));

    // Drain all
    std::vector<uint64_t> expected;
    for (uint64_t i = 8; i < 24; ++i) expected.push_back(i);
    for (uint64_t exp : expected) {
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, exp);
    }
    EXPECT_FALSE(q.pop(v));
}

TEST(SPSCSingleThreaded, SizeApprox) {
    SPSCQueue<int, 8> q;
    EXPECT_EQ(q.size_approx(), 0u);
    EXPECT_TRUE(q.empty_approx());

    q.push(1);
    EXPECT_EQ(q.size_approx(), 1u);
    EXPECT_FALSE(q.empty_approx());

    int v;
    q.pop(v);
    EXPECT_EQ(q.size_approx(), 0u);
}

TEST(SPSCSingleThreaded, CapacityConstant) {
    EXPECT_EQ((SPSCQueue<int, 64>::capacity()), 64u);
}

TEST(SPSCSingleThreaded, RvalueMove) {
    SPSCQueue<int, 8> q;
    int x = 42;
    EXPECT_TRUE(q.push(std::move(x)));
    int v;
    EXPECT_TRUE(q.pop(v));
    EXPECT_EQ(v, 42);
}

// ============================================================================
// Multi-threaded correctness: no loss, no duplication, FIFO order maintained
// ============================================================================

TEST(SPSCMultiThreaded, NoLossNoDuplicate_1M) {
    static constexpr int N = 1'000'000;
    SPSCQueue<uint64_t, 4096> q;
    std::vector<uint64_t> consumed;
    consumed.reserve(N);

    std::thread producer([&] {
        for (uint64_t i = 0; i < N; ++i)
            while (!q.push(i)) { /* spin — queue full, wait */ }
    });

    std::thread consumer([&] {
        uint64_t item;
        for (int i = 0; i < N; ++i) {
            while (!q.pop(item)) { /* spin — queue empty, wait */ }
            consumed.push_back(item);
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(static_cast<int>(consumed.size()), N);
    for (int i = 0; i < N; ++i)
        EXPECT_EQ(consumed[static_cast<std::size_t>(i)], static_cast<uint64_t>(i))
            << "FIFO violated at index " << i;
}

TEST(SPSCMultiThreaded, NoLossNoDuplicate_10M) {
    static constexpr int N = 10'000'000;
    SPSCQueue<uint64_t, 4096> q;
    std::vector<uint64_t> consumed;
    consumed.reserve(N);

    std::thread producer([&] {
        for (uint64_t i = 0; i < N; ++i)
            while (!q.push(i)) { /* spin */ }
    });

    std::thread consumer([&] {
        uint64_t item;
        for (int i = 0; i < N; ++i) {
            while (!q.pop(item)) { /* spin */ }
            consumed.push_back(item);
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(static_cast<int>(consumed.size()), N);
    for (int i = 0; i < N; ++i)
        EXPECT_EQ(consumed[static_cast<std::size_t>(i)], static_cast<uint64_t>(i));
}

TEST(SPSCMultiThreaded, SmallQueueHighContention) {
    // Capacity = 2 maximises the chance of hitting full/empty conditions
    static constexpr int N = 100'000;
    SPSCQueue<int, 2> q;
    std::vector<int> results;
    results.reserve(N);

    std::thread prod([&] {
        for (int i = 0; i < N; ++i)
            while (!q.push(i)) {}
    });

    std::thread cons([&] {
        int v;
        for (int i = 0; i < N; ++i) {
            while (!q.pop(v)) {}
            results.push_back(v);
        }
    });

    prod.join();
    cons.join();

    ASSERT_EQ(static_cast<int>(results.size()), N);
    for (int i = 0; i < N; ++i)
        EXPECT_EQ(results[static_cast<std::size_t>(i)], i);
}

// ============================================================================
// Compile-time constraint checks
// ============================================================================

// These should FAIL to compile — uncomment to verify:
//   struct NonTrivial { std::string s; };
//   SPSCQueue<NonTrivial, 8> bad;  // static_assert: T must be trivially copyable

// static_assert((SPSCQueue<int, 3>::capacity()) == 3); // should fail: not power-of-2
