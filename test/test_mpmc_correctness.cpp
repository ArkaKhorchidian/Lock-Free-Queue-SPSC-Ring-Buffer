#include <gtest/gtest.h>
#include "mpmc_queue.hpp"

#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>
#include <numeric>
#include <cstdint>
#include <mutex>

// ============================================================================
// Single-threaded correctness
// ============================================================================

TEST(MPMCSingleThreaded, PushAndPopFIFO) {
    MPMCQueue<int, 8> q;
    EXPECT_TRUE(q.push(10));
    EXPECT_TRUE(q.push(20));
    EXPECT_TRUE(q.push(30));

    int v;
    EXPECT_TRUE(q.pop(v));  EXPECT_EQ(v, 10);
    EXPECT_TRUE(q.pop(v));  EXPECT_EQ(v, 20);
    EXPECT_TRUE(q.pop(v));  EXPECT_EQ(v, 30);
    EXPECT_FALSE(q.pop(v));
}

TEST(MPMCSingleThreaded, PushToFull) {
    MPMCQueue<int, 4> q;
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.push(3));
    EXPECT_TRUE(q.push(4));
    EXPECT_FALSE(q.push(5)); // full
}

TEST(MPMCSingleThreaded, WrapAround) {
    MPMCQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(q.push(i));

    int v;
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);
    }
    ASSERT_TRUE(q.push(10));
    ASSERT_TRUE(q.push(11));

    // Expected order: 2, 3, 10, 11
    const int expected[] = {2, 3, 10, 11};
    for (int exp : expected) {
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, exp);
    }
    EXPECT_FALSE(q.pop(v));
}

// ============================================================================
// Multi-threaded correctness: 1P1C, 2P2C, 4P4C
// ============================================================================

// Helper: run N_PRODUCERS producers and N_CONSUMERS consumers.
// Each producer sends a disjoint range of integers.
// Verify: total items received == total sent, no duplicates, no missed items.
template<int N_PRODUCERS, int N_CONSUMERS, std::size_t CAP>
void run_mpmc_test(int items_per_producer) {
    MPMCQueue<int, CAP> q;

    const int total = N_PRODUCERS * items_per_producer;
    std::vector<std::atomic<int>> counts(static_cast<std::size_t>(total));
    for (auto& c : counts) c.store(0);

    std::vector<std::thread> producers, consumers;
    std::atomic<int> consumer_count{0};

    for (int p = 0; p < N_PRODUCERS; ++p) {
        producers.emplace_back([&, p] {
            const int base = p * items_per_producer;
            for (int i = 0; i < items_per_producer; ++i)
                while (!q.push(base + i)) { /* spin */ }
        });
    }

    for (int c = 0; c < N_CONSUMERS; ++c) {
        consumers.emplace_back([&] {
            int v;
            for (;;) {
                if (q.pop(v)) {
                    counts[static_cast<std::size_t>(v)].fetch_add(1, std::memory_order_relaxed);
                    if (consumer_count.fetch_add(1, std::memory_order_relaxed) + 1 == total)
                        return;
                } else if (consumer_count.load(std::memory_order_relaxed) == total) {
                    return;
                }
            }
        });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    EXPECT_EQ(consumer_count.load(), total);
    for (int i = 0; i < total; ++i)
        EXPECT_EQ(counts[static_cast<std::size_t>(i)].load(), 1)
            << "Item " << i << " received " << counts[static_cast<std::size_t>(i)].load() << " times";
}

TEST(MPMCMultiThreaded, OneProducerOneConsumer_100K) {
    run_mpmc_test<1, 1, 1024>(100'000);
}

TEST(MPMCMultiThreaded, TwoProducersTwoConsumers_100K) {
    run_mpmc_test<2, 2, 1024>(100'000);
}

TEST(MPMCMultiThreaded, FourProducersFourConsumers_50K) {
    run_mpmc_test<4, 4, 1024>(50'000);
}

TEST(MPMCMultiThreaded, EightProducersEightConsumers_10K) {
    run_mpmc_test<8, 8, 2048>(10'000);
}

TEST(MPMCMultiThreaded, AsymmetricTwoProducersOneConsumer) {
    run_mpmc_test<2, 1, 512>(50'000);
}

TEST(MPMCMultiThreaded, AsymmetricOneProducerFourConsumers) {
    run_mpmc_test<1, 4, 512>(100'000);
}
