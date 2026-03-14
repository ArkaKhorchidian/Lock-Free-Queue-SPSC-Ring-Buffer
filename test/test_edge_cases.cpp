#include <gtest/gtest.h>
#include "spsc_queue.hpp"
#include "mpmc_queue.hpp"
#include "cache_line.hpp"

#include <cstdint>
#include <type_traits>
#include <array>

// ============================================================================
// Edge cases: boundary conditions, layout, compile-time guarantees
// ============================================================================

// ---------------------------------------------------------------------------
// Cache line layout
// ---------------------------------------------------------------------------

TEST(CacheLine, SizeIsMultipleOf64OrDetected) {
    // CACHE_LINE_SIZE should be a power of 2 and at least 64
    EXPECT_GE(CACHE_LINE_SIZE, 64u);
    EXPECT_EQ(CACHE_LINE_SIZE & (CACHE_LINE_SIZE - 1), 0u); // power of 2
}

TEST(CacheLine, CacheLineAlignedSize) {
    // CacheLineAligned<char> must be exactly one cache line
    EXPECT_EQ(sizeof(CacheLineAligned<char>), CACHE_LINE_SIZE);

    // CacheLineAligned<double[8]> (64 bytes) must be exactly one cache line
    // (assuming CACHE_LINE_SIZE == 64; otherwise at least one cache line)
    EXPECT_GE(sizeof(CacheLineAligned<std::array<double, 8>>), CACHE_LINE_SIZE);
    EXPECT_EQ(sizeof(CacheLineAligned<std::array<double, 8>>) % CACHE_LINE_SIZE, 0u);
}

TEST(CacheLine, CacheLineAlignedAlignment) {
    EXPECT_EQ(alignof(CacheLineAligned<int>), CACHE_LINE_SIZE);
}

// ---------------------------------------------------------------------------
// SPSCQueue — static assertions and layout
// ---------------------------------------------------------------------------

TEST(SPSCEdge, HeadAndTailOnSeparateCacheLines) {
    // The struct layout must place head_ and tail_ at least CACHE_LINE_SIZE
    // bytes apart to eliminate false sharing.  We verify by inspecting
    // offsetof — the compiler guarantees the alignas places them correctly.
    //
    // We use a concrete instantiation with a small T to make the offsets
    // predictable, then check that the gap is at least CACHE_LINE_SIZE.
    using Q = SPSCQueue<int, 8>;

    // offsetof is not directly available for non-standard-layout types in C++.
    // Instead, verify that the total size is consistent with separate-line layout.
    // Minimum expected size: at least 3 cache lines (tail_, head_, buffer_).
    EXPECT_GE(sizeof(Q), 3 * CACHE_LINE_SIZE);
}

TEST(SPSCEdge, MinimumCapacity_2) {
    // Capacity = 2 is the smallest valid power-of-2 queue
    SPSCQueue<int, 2> q;
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_FALSE(q.push(3)); // full

    int v;
    EXPECT_TRUE(q.pop(v));  EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.pop(v));  EXPECT_EQ(v, 2);
    EXPECT_FALSE(q.pop(v)); // empty
}

TEST(SPSCEdge, MaximalCapacity_LargePowerOf2) {
    // Large capacity — verify indexing doesn't overflow with large counts
    SPSCQueue<uint64_t, 65536> q;
    EXPECT_EQ(q.capacity(), 65536u);

    // Push half, pop half, push again (exercises wraparound at large indices)
    for (uint64_t i = 0; i < 32768; ++i) ASSERT_TRUE(q.push(i));

    uint64_t v;
    for (uint64_t i = 0; i < 32768; ++i) {
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);
    }

    for (uint64_t i = 32768; i < 65536; ++i) ASSERT_TRUE(q.push(i));
    for (uint64_t i = 32768; i < 65536; ++i) {
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);
    }
}

TEST(SPSCEdge, RepeatFillDrainPreservesCorrectness) {
    // Fill and drain the queue N times — ensures the sequence counter
    // (tail_ - head_) comparison works across many wrap-around cycles
    SPSCQueue<int, 4> q;
    for (int round = 0; round < 1000; ++round) {
        for (int i = 0; i < 4; ++i) ASSERT_TRUE(q.push(round * 4 + i));
        ASSERT_FALSE(q.push(-1)); // full

        int v;
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(q.pop(v));
            EXPECT_EQ(v, round * 4 + i);
        }
        ASSERT_FALSE(q.pop(v)); // empty
    }
}

TEST(SPSCEdge, PushRvalueAndLvalue) {
    SPSCQueue<uint64_t, 8> q;

    const uint64_t lval = 0xDEADBEEF;
    EXPECT_TRUE(q.push(lval));

    uint64_t rval = 0xCAFEBABE;
    EXPECT_TRUE(q.push(std::move(rval)));

    uint64_t out;
    EXPECT_TRUE(q.pop(out)); EXPECT_EQ(out, 0xDEADBEEF);
    EXPECT_TRUE(q.pop(out)); EXPECT_EQ(out, 0xCAFEBABE);
}

// ---------------------------------------------------------------------------
// MPMCQueue edge cases
// ---------------------------------------------------------------------------

TEST(MPMCEdge, MinimumCapacity) {
    MPMCQueue<int, 2> q;
    EXPECT_TRUE(q.push(42));
    EXPECT_TRUE(q.push(99));
    EXPECT_FALSE(q.push(0)); // full

    int v;
    EXPECT_TRUE(q.pop(v));  EXPECT_EQ(v, 42);
    EXPECT_TRUE(q.push(7)); // now has space again
    EXPECT_TRUE(q.pop(v));  EXPECT_EQ(v, 99);
    EXPECT_TRUE(q.pop(v));  EXPECT_EQ(v, 7);
    EXPECT_FALSE(q.pop(v)); // empty
}

TEST(MPMCEdge, RepeatFillDrain) {
    MPMCQueue<int, 8> q;
    for (int round = 0; round < 500; ++round) {
        for (int i = 0; i < 8; ++i) ASSERT_TRUE(q.push(i));
        ASSERT_FALSE(q.push(99)); // full

        int v;
        for (int i = 0; i < 8; ++i) {
            ASSERT_TRUE(q.pop(v));
            EXPECT_EQ(v, i);
        }
        ASSERT_FALSE(q.pop(v)); // empty
    }
}

TEST(MPMCEdge, SizeApprox) {
    MPMCQueue<int, 16> q;
    EXPECT_EQ(q.size_approx(), 0u);
    q.push(1);
    q.push(2);
    EXPECT_EQ(q.size_approx(), 2u);
}

// ---------------------------------------------------------------------------
// Compile-time type constraints (static_assert checks)
// ---------------------------------------------------------------------------

// These lines verify the static_asserts fire for invalid configurations.
// They are left as comments — uncomment one at a time to verify the error.

// Non-power-of-2 capacity — should fail to compile:
// SPSCQueue<int, 3> bad_cap;

// Non-trivially-copyable type — should fail to compile:
// struct NonTrivial { std::string s; };
// SPSCQueue<NonTrivial, 8> bad_type;

// Same for MPMC:
// MPMCQueue<int, 3> bad_mpmc_cap;
// MPMCQueue<NonTrivial, 8> bad_mpmc_type;

// Verify trivially_copyable static_assert holds for built-in types
static_assert(std::is_trivially_copyable_v<int>);
static_assert(std::is_trivially_copyable_v<uint64_t>);
static_assert(std::is_trivially_copyable_v<float>);
static_assert(std::is_trivially_copyable_v<double>);
