#include "mpmc_queue.hpp"
#include "latency_harness.hpp"

#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

static void pin_thread(int core_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
    (void)core_id;
#endif
}

// ---------------------------------------------------------------------------
// MPMC Throughput Scaling Benchmark
//
// Measures throughput at 1P1C, 2P2C, 4P4C, 8P8C configurations.
//
// Expected behaviour:
//   - Throughput scales approximately linearly from 1P1C to ~4P4C.
//   - Beyond 4P4C, CAS contention on enqueue_pos_ and dequeue_pos_ becomes
//     the bottleneck and throughput flattens.
//   - This is expected — the Vyukov MPMC queue is not designed for 16+ thread
//     contention.  For that scale, per-thread sharding is more appropriate.
// ---------------------------------------------------------------------------

static void bench_mpmc_scaling(int n_producers, int n_consumers,
                                int items_per_producer) {
    static constexpr std::size_t Q_CAP = 65536;
    MPMCQueue<uint64_t, Q_CAP> q;

    const int total_items = n_producers * items_per_producer;
    std::atomic<int> consumed{0};

    using Clock = std::chrono::steady_clock;
    Clock::time_point start_time, end_time;

    std::vector<std::thread> producers, consumers;

    // Start consumers first to avoid early queue-full spinning
    std::atomic<bool> start_flag{false};

    for (int c = 0; c < n_consumers; ++c) {
        consumers.emplace_back([&, c] {
            pin_thread(c % 8 + n_producers); // pin after producer cores
            while (!start_flag.load(std::memory_order_acquire)) {}
            uint64_t item;
            for (;;) {
                if (q.pop(item)) {
                    const int cnt = consumed.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (cnt == total_items) {
                        end_time = Clock::now();
                        return;
                    }
                } else if (consumed.load(std::memory_order_relaxed) == total_items) {
                    return;
                }
            }
        });
    }

    for (int p = 0; p < n_producers; ++p) {
        producers.emplace_back([&, p] {
            pin_thread(p % 8); // pin producers to first 8 cores
            while (!start_flag.load(std::memory_order_acquire)) {}
            const uint64_t base = static_cast<uint64_t>(p) * items_per_producer;
            for (int i = 0; i < items_per_producer; ++i)
                while (!q.push(base + static_cast<uint64_t>(i))) {}
        });
    }

    start_time = Clock::now();
    start_flag.store(true, std::memory_order_release);

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    const double elapsed_s = std::chrono::duration<double>(end_time - start_time).count();
    const double ops_per_sec = total_items / elapsed_s;

    std::printf("  %dP%dC: %8.1f M ops/sec  (%.3f s, %d items)\n",
                n_producers, n_consumers,
                ops_per_sec / 1e6,
                elapsed_s,
                total_items);
}

int main() {
    static constexpr int ITEMS_PER_PRODUCER = 2'000'000;

    std::printf("=== MPMC Throughput Scaling ===\n");
    std::printf("  (Queue capacity: 65536, items per producer: %d)\n\n",
                ITEMS_PER_PRODUCER);

    bench_mpmc_scaling(1, 1, ITEMS_PER_PRODUCER);
    bench_mpmc_scaling(2, 2, ITEMS_PER_PRODUCER);
    bench_mpmc_scaling(4, 4, ITEMS_PER_PRODUCER);

    // 8P8C requires at least 16 cores; skip if the machine has fewer.
#ifdef __linux__
    const int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (hw_threads >= 16) {
        bench_mpmc_scaling(8, 8, ITEMS_PER_PRODUCER / 4);
    } else {
        std::printf("  8P8C: skipped (hardware_concurrency = %d < 16)\n", hw_threads);
    }
#else
    std::printf("  8P8C: skipped (core pinning not supported on this platform)\n");
#endif

    std::printf("\n");
    std::printf("Expected: throughput scales ~linearly to 4P4C, then flattens\n");
    std::printf("due to CAS contention on enqueue_pos_/dequeue_pos_.\n");

    return 0;
}
