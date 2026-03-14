#include <gtest/gtest.h>
#include "spsc_queue.hpp"

#include <thread>
#include <vector>
#include <cstdint>

// ============================================================================
// ThreadSanitizer stress tests
//
// These tests are specifically designed to exercise the memory ordering paths
// that TSan validates.  Run this suite with -fsanitize=thread; a correct
// implementation emits zero warnings.
//
// TSan instruments every memory access and tracks synchronisation edges
// (acquire/release, mutex lock/unlock, thread create/join).  If it finds a
// location that is accessed by two threads with no synchronisation edge
// between the accesses, it reports a data race.
//
// A lock-free implementation that fails TSan is WRONG — not "probably fine".
// The C++ memory model gives compilers and hardware latitude to reorder
// unsynchronised accesses in ways that are non-obvious on x86 but guaranteed
// to appear on ARM/POWER.  TSan's model correctly reflects this.
// ============================================================================

// High-iteration stress: push/pop 20M items, exercising every code path
TEST(SPSCTSan, StressTest_20M) {
    static constexpr int N = 20'000'000;
    SPSCQueue<uint64_t, 8192> q;

    std::thread producer([&] {
        for (uint64_t i = 0; i < N; ++i)
            while (!q.push(i)) {}
    });

    std::thread consumer([&] {
        uint64_t item, expected = 0;
        while (expected < N) {
            if (q.pop(item)) {
                ASSERT_EQ(item, expected) << "FIFO violated at sequence " << expected;
                ++expected;
            }
        }
    });

    producer.join();
    consumer.join();
}

// Stress with a very small queue to maximise full/empty transitions
TEST(SPSCTSan, SmallQueue_StressTest) {
    static constexpr int N = 1'000'000;
    SPSCQueue<uint32_t, 4> q;

    std::thread producer([&] {
        for (uint32_t i = 0; i < N; ++i)
            while (!q.push(i)) {}
    });

    std::thread consumer([&] {
        uint32_t item, expected = 0;
        while (expected < N) {
            if (q.pop(item)) {
                ASSERT_EQ(item, expected);
                ++expected;
            }
        }
    });

    producer.join();
    consumer.join();
}

// Multiple independent queue instances (tests that separate instances don't
// interfere with each other under TSan's happens-before tracking)
TEST(SPSCTSan, MultipleIndependentQueues) {
    static constexpr int N = 200'000;
    static constexpr int NUM_QUEUES = 4;

    struct QueuePair {
        SPSCQueue<uint64_t, 256> q;
        std::vector<uint64_t> results;
    };

    std::vector<QueuePair> pairs(NUM_QUEUES);
    for (auto& p : pairs) p.results.reserve(N);

    std::vector<std::thread> producers, consumers;

    for (int qi = 0; qi < NUM_QUEUES; ++qi) {
        producers.emplace_back([&, qi] {
            for (uint64_t i = 0; i < N; ++i)
                while (!pairs[static_cast<std::size_t>(qi)].q.push(i)) {}
        });
        consumers.emplace_back([&, qi] {
            uint64_t item;
            for (int i = 0; i < N; ++i) {
                while (!pairs[static_cast<std::size_t>(qi)].q.pop(item)) {}
                pairs[static_cast<std::size_t>(qi)].results.push_back(item);
            }
        });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    for (int qi = 0; qi < NUM_QUEUES; ++qi) {
        const auto& r = pairs[static_cast<std::size_t>(qi)].results;
        ASSERT_EQ(static_cast<int>(r.size()), N);
        for (int i = 0; i < N; ++i)
            EXPECT_EQ(r[static_cast<std::size_t>(i)], static_cast<uint64_t>(i));
    }
}

// Burst pattern: producer fills the queue completely, consumer drains it,
// repeated many times.  This exercises the boundary where tail wraps around.
TEST(SPSCTSan, BurstFillDrain) {
    static constexpr int ITERATIONS = 10'000;
    static constexpr std::size_t Q_CAP = 64;
    SPSCQueue<int, Q_CAP> q;

    std::atomic<bool> done{false};
    std::atomic<int>  total_consumed{0};

    std::thread producer([&] {
        for (int burst = 0; burst < ITERATIONS; ++burst) {
            for (int i = 0; i < static_cast<int>(Q_CAP); ++i)
                while (!q.push(burst * static_cast<int>(Q_CAP) + i)) {}
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        int item;
        int expected = 0;
        while (!done.load(std::memory_order_acquire) ||
               total_consumed.load(std::memory_order_relaxed) < ITERATIONS * static_cast<int>(Q_CAP)) {
            if (q.pop(item)) {
                ASSERT_EQ(item, expected++);
                total_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(total_consumed.load(), ITERATIONS * static_cast<int>(Q_CAP));
}
