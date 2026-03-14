#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <type_traits>
#include <new>      // std::hardware_destructive_interference_size

#include "cache_line.hpp"

// ---------------------------------------------------------------------------
// SPSCQueue<T, Capacity>
//
// A single-producer single-consumer (SPSC) lock-free ring buffer.
//
// CONTRACT: exactly one thread calls push() and exactly one (different) thread
// calls pop() at any time.  Violating this contract is undefined behaviour —
// the implementation makes no attempt to detect or handle it.
//
// The design exploits the SPSC constraint to minimise synchronisation:
//   - Only two atomic variables are needed: head_ (consumer-owned) and
//     tail_ (producer-owned).
//   - Each variable is written by exactly one thread, so the owning thread
//     can read its own variable with memory_order_relaxed (no fence required).
//   - They are placed on separate cache lines to eliminate false sharing,
//     which is the dominant performance factor in a tight push/pop loop.
//
// Capacity must be a power of 2.  This allows index masking (& MASK) instead
// of modulo, which matters: modulo is a division and compilers cannot always
// optimise it away even with a constant divisor.
//
// T must be trivially copyable.  Lock-free code writes to shared memory
// directly; non-trivial copy/move constructors may internally allocate or
// use mutexes, defeating the purpose.
// ---------------------------------------------------------------------------

template<typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "SPSCQueue: Capacity must be a power of 2");
    static_assert(Capacity >= 2,
                  "SPSCQueue: Capacity must be at least 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSCQueue: T must be trivially copyable for lock-free correctness. "
                  "Non-trivial constructors may internally synchronise (e.g. allocate), "
                  "introducing hidden data races.");

public:
    SPSCQueue() = default;

    // Non-copyable, non-movable.  Moving a queue while a thread is using it
    // is a data race; forbidding it removes an entire class of bugs.
    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&)                 = delete;
    SPSCQueue& operator=(SPSCQueue&&)      = delete;

    // -----------------------------------------------------------------------
    // push() — producer thread only
    //
    // Returns true if the item was enqueued, false if the queue was full.
    // Guaranteed allocation-free and noexcept.
    // -----------------------------------------------------------------------
    bool push(const T& item) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        // memory_order_relaxed: the producer is the only writer of tail_, so
        // the producer's own view of tail_ is always coherent.  No
        // synchronisation with any other thread is needed here.

        const auto head = head_.load(std::memory_order_acquire);
        // memory_order_acquire: pairs with the consumer's release store to
        // head_ in pop().  This establishes a happens-before edge:
        //   consumer's writes before head_.store(release)
        //     happens-before
        //   producer's reads after head_.load(acquire)
        //
        // Concretely: after acquiring head_, we are guaranteed to observe that
        // the slot at (head & MASK) has already been read by the consumer and
        // is safe to overwrite.

        if (tail - head == Capacity) return false; // queue full

        buffer_[tail & MASK] = item;
        // The write to buffer_ must be visible to the consumer before the
        // consumer reads tail_.  The release store below provides this
        // ordering: any store that precedes a release store is visible to any
        // thread that subsequently performs an acquire load of the same atomic.

        tail_.store(tail + 1, std::memory_order_release);
        // memory_order_release: pairs with the consumer's acquire load of
        // tail_ in pop().  Guarantees the buffer_ write above is visible to
        // the consumer when it observes the updated tail_.
        return true;
    }

    bool push(T&& item) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        if (tail - head == Capacity) return false;
        buffer_[tail & MASK] = std::move(item);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // -----------------------------------------------------------------------
    // pop() — consumer thread only
    //
    // Returns true and fills item if an element was dequeued; false if empty.
    // Guaranteed allocation-free and noexcept.
    // -----------------------------------------------------------------------
    bool pop(T& item) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        // memory_order_relaxed: symmetric reasoning to push().  The consumer
        // is the only writer of head_, so its own view is always coherent.

        const auto tail = tail_.load(std::memory_order_acquire);
        // memory_order_acquire: pairs with the producer's release store to
        // tail_ in push().  Establishes:
        //   producer's buffer_ write happens-before
        //   consumer's buffer_ read below.
        //
        // Without this acquire, the CPU (or compiler) could legally reorder
        // the buffer_ read before the tail_ load, reading a slot that the
        // producer has not yet written — a data race.

        if (head == tail) return false; // queue empty

        item = buffer_[head & MASK];
        // Safe to read: we acquired tail_, which synchronised with the
        // producer's release of tail_ after writing to buffer_[head & MASK].

        head_.store(head + 1, std::memory_order_release);
        // memory_order_release: pairs with the producer's acquire load of
        // head_ in push().  Signals to the producer that slot (head & MASK)
        // has been consumed and is now free to be overwritten.
        return true;
    }

    // -----------------------------------------------------------------------
    // Approximate state queries
    //
    // These are approximate: another thread may concurrently modify the queue.
    // Do NOT use the result to make correctness decisions — use only as hints
    // (e.g., for back-pressure metrics or adaptive spinning).
    // -----------------------------------------------------------------------
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_relaxed);
        return tail - head; // wraps correctly in unsigned arithmetic
    }

    [[nodiscard]] bool empty_approx() const noexcept {
        return size_approx() == 0;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::size_t MASK = Capacity - 1;

    // -----------------------------------------------------------------------
    // Cache-line layout
    //
    // tail_ is written by the producer.
    // head_ is written by the consumer.
    //
    // If both were on the same cache line, every write by one thread would
    // invalidate the cache line in the other thread's L1 cache (false sharing),
    // forcing a cross-core cache coherence round-trip (~60–200 ns on modern
    // x86) on every push/pop.  Placing them on separate lines eliminates this.
    //
    // The buffer_ is also placed on its own line to avoid sharing a line with
    // head_ or tail_ when Capacity is small.
    // -----------------------------------------------------------------------

    // Producer-owned: written exclusively by the producer thread
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> tail_{0};

    // Consumer-owned: written exclusively by the consumer thread
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> head_{0};

    // Data storage: accessed by both threads at disjoint indices
    alignas(CACHE_LINE_SIZE) std::array<T, Capacity> buffer_{};
};
