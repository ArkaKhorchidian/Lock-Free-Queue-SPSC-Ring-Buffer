#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <type_traits>

#include "cache_line.hpp"

// ---------------------------------------------------------------------------
// MPMCQueue<T, Capacity>
//
// Bounded multi-producer multi-consumer lock-free queue based on Dmitry
// Vyukov's design (2010):
//   http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
//
// This is the canonical bounded MPMC queue in production systems.  It avoids
// the ABA problem without tagged pointers by giving each cell its own
// monotonically-increasing sequence counter.
//
// KEY INSIGHT — the sequence counter approach:
//   Each cell holds a sequence number that tracks the cell's "generation".
//   A cell is ready to be written when sequence == enqueue_pos (the cell is
//   in the right generation for a new write).  A cell is ready to be read
//   when sequence == dequeue_pos + 1 (it has been written and is waiting
//   for a reader).  After a dequeue, the sequence is set to
//   dequeue_pos + Capacity, reserving the cell for the next generation's write.
//
// ABA avoidance:
//   Classic ABA: a pointer is read as A, another thread changes it to B then
//   back to A, and a CAS mistakenly succeeds.  Here, sequence counters are
//   monotonically increasing — a cell in generation N cannot be confused with
//   the same cell in generation N+2 because the sequence number differs.
//
// Performance characteristics:
//   - Scales to ~4 producers / 4 consumers before CAS contention on
//     enqueue_pos_ / dequeue_pos_ becomes the bottleneck.
//   - Beyond that, throughput flattens (not degrades) because losers in the
//     CAS retry rather than blocking.
//   - On x86-64, compare_exchange_weak compiles to LOCK CMPXCHG, which is
//     cheaper than a full memory fence but still a bus-locked operation.
//
// CONTRACT:
//   Capacity must be a power of 2.
//   T must be trivially copyable (same reasoning as SPSCQueue).
// ---------------------------------------------------------------------------

template<typename T, std::size_t Capacity>
class MPMCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "MPMCQueue: Capacity must be a power of 2");
    static_assert(Capacity >= 2,
                  "MPMCQueue: Capacity must be at least 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "MPMCQueue: T must be trivially copyable");

    // -----------------------------------------------------------------------
    // Cell — one slot in the ring buffer
    //
    // The sequence counter is on its own cache line to prevent false sharing
    // between adjacent cells when multiple producers write to adjacent slots.
    // Without this padding, a producer writing sequence[i] would invalidate
    // the cache line also containing sequence[i+1], which is being read by
    // another producer — classic false sharing in a tight loop.
    //
    // NOTE: For small T (e.g. uint64_t), this wastes memory.  A production
    // variant might use a separate sequence array.  For portfolio purposes,
    // clarity and correctness take precedence.
    // -----------------------------------------------------------------------
    struct alignas(CACHE_LINE_SIZE) Cell {
        std::atomic<std::size_t> sequence;
        T data;
    };

public:
    MPMCQueue() {
        // Initialise each cell's sequence to its index.
        // Cell i is ready to be written by enqueue_pos == i (diff == 0).
        for (std::size_t i = 0; i < Capacity; ++i)
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        // memory_order_relaxed: these stores happen before any thread can
        // access the queue (constructor precedes any concurrent use).
        // No synchronisation with other threads is needed here.
    }

    MPMCQueue(const MPMCQueue&)            = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;
    MPMCQueue(MPMCQueue&&)                 = delete;
    MPMCQueue& operator=(MPMCQueue&&)      = delete;

    // -----------------------------------------------------------------------
    // push() — any producer thread
    //
    // Returns true on success, false if the queue is full (non-blocking).
    // -----------------------------------------------------------------------
    bool push(const T& item) noexcept {
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        // memory_order_relaxed: we only use pos as a starting hint for the
        // CAS loop.  The CAS itself provides the necessary ordering.

        for (;;) {
            Cell& cell = buffer_[pos & MASK];

            const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
            // memory_order_acquire: synchronise with the consumer's release
            // store that set sequence = pos + Capacity after dequeuing.
            // Ensures we see: the cell is genuinely free to write (not still
            // being read by a consumer in a prior generation).

            const auto diff = static_cast<std::ptrdiff_t>(seq)
                            - static_cast<std::ptrdiff_t>(pos);

            if (diff == 0) {
                // Cell is in the right generation — claim it with a CAS.
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    // We own this cell.  Write the data, then publish by
                    // storing sequence = pos + 1 with release ordering.
                    cell.data = item;
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    // memory_order_release: pairs with the consumer's
                    // acquire load of cell.sequence in pop().  Guarantees
                    // the consumer sees the cell.data write above.
                    return true;
                }
                // CAS failed — another producer claimed this slot.  Retry
                // with the updated pos (written back by CAS on failure).
            } else if (diff < 0) {
                // seq < pos: the cell's sequence is behind our position.
                // This means the cell has not yet been freed by a consumer
                // after the previous generation — the queue is full.
                return false;
            } else {
                // diff > 0: another producer already advanced enqueue_pos_
                // past pos.  Reload and retry.
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    bool push(T&& item) noexcept {
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = buffer_[pos & MASK];
            const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq)
                            - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                    cell.data = std::move(item);
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    // -----------------------------------------------------------------------
    // pop() — any consumer thread
    //
    // Returns true and fills item on success; false if queue is empty.
    // -----------------------------------------------------------------------
    bool pop(T& item) noexcept {
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        // memory_order_relaxed: starting hint for CAS loop.

        for (;;) {
            Cell& cell = buffer_[pos & MASK];

            const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
            // memory_order_acquire: pairs with the producer's release store
            // of sequence = pos + 1 in push().  Ensures we see the data
            // write that preceded the sequence update.

            const auto diff = static_cast<std::ptrdiff_t>(seq)
                            - static_cast<std::ptrdiff_t>(pos + 1);

            if (diff == 0) {
                // Cell is ready to be read — claim it.
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    item = cell.data;
                    // Advance sequence by Capacity to mark the slot ready
                    // for the next generation of writes.
                    cell.sequence.store(pos + Capacity, std::memory_order_release);
                    // memory_order_release: pairs with the next producer's
                    // acquire load of cell.sequence.  Guarantees the producer
                    // sees this slot as free for generation (pos + Capacity).
                    return true;
                }
                // CAS failed — another consumer claimed this slot.
            } else if (diff < 0) {
                // seq < pos + 1: producer hasn't written yet — queue is empty.
                return false;
            } else {
                // diff > 0: another consumer advanced past pos.  Retry.
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const auto enq = enqueue_pos_.load(std::memory_order_relaxed);
        const auto deq = dequeue_pos_.load(std::memory_order_relaxed);
        return enq > deq ? enq - deq : 0;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::size_t MASK = Capacity - 1;

    alignas(CACHE_LINE_SIZE) std::array<Cell, Capacity> buffer_;
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t>   enqueue_pos_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t>   dequeue_pos_{0};
};
