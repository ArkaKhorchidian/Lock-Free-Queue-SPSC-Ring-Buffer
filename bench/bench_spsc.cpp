#include "spsc_queue.hpp"
#include "latency_harness.hpp"

#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
#include <numeric>
#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

// ---------------------------------------------------------------------------
// CPU pinning helpers
//
// Pinning the producer and consumer to separate physical cores eliminates
// OS scheduler jitter and cross-NUMA cache traffic.  Without pinning,
// latency measurements have high variance because the OS may schedule both
// threads on the same core (serialising them) or on different NUMA nodes
// (adding 100+ ns of remote memory access latency).
//
// These calls are Linux-specific.  On macOS, thread affinity is advisory and
// the kernel may ignore it.  Cross-NUMA measurements are left as future work.
// ---------------------------------------------------------------------------

static void pin_thread(int core_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
    (void)core_id; // macOS: no-op
#endif
}

// ---------------------------------------------------------------------------
// Payload: carries a send timestamp so we can measure one-way latency.
// Must be trivially copyable for use with SPSCQueue.
// ---------------------------------------------------------------------------
struct TimedItem {
    uint64_t seq;       // sequence number for FIFO verification
    uint64_t send_tsc;  // RDTSC at point of push()
};
static_assert(std::is_trivially_copyable_v<TimedItem>);

// ---------------------------------------------------------------------------
// Benchmark 1: Throughput — sustained ops/sec at 100% load
//
// Producer pushes as fast as possible; consumer pops as fast as possible.
// Measure total ops / wall-clock seconds.
// ---------------------------------------------------------------------------
static void bench_throughput(int producer_core, int consumer_core) {
    static constexpr int N = 50'000'000;
    SPSCQueue<uint64_t, 65536> q;

    using Clock = std::chrono::steady_clock;
    Clock::time_point start_time, end_time;

    std::thread producer([&] {
        pin_thread(producer_core);
        start_time = Clock::now();
        for (uint64_t i = 0; i < N; ++i)
            while (!q.push(i)) {}
    });

    std::thread consumer([&] {
        pin_thread(consumer_core);
        uint64_t item;
        for (int i = 0; i < N; ++i)
            while (!q.pop(item)) {}
        end_time = Clock::now();
    });

    producer.join();
    consumer.join();

    const double elapsed_s = std::chrono::duration<double>(end_time - start_time).count();
    const double ops_per_sec = N / elapsed_s;

    std::printf("=== SPSC Throughput ===\n");
    std::printf("  Items:        %d\n", N);
    std::printf("  Time:         %.3f s\n", elapsed_s);
    std::printf("  Throughput:   %.1f M ops/sec\n", ops_per_sec / 1e6);
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// Benchmark 2: Round-trip latency histogram
//
// Producer pushes a TimedItem with send_tsc set.
// Consumer pops and records (recv_tsc - send_tsc) as one-way latency.
// Note: one-way latency is approximated; true one-way requires synchronised
// clocks.  On a single machine with TSC sync, this is accurate to ~10 cycles.
// ---------------------------------------------------------------------------
static void bench_latency(int producer_core, int consumer_core) {
    static constexpr int WARMUP   = 100'000;
    static constexpr int MEASURED = 1'000'000;
    static constexpr int TOTAL    = WARMUP + MEASURED;

    SPSCQueue<TimedItem, 256> q;
    std::vector<uint64_t> latencies;
    latencies.reserve(MEASURED);

    std::thread producer([&] {
        pin_thread(producer_core);
        for (int i = 0; i < TOTAL; ++i) {
            TimedItem item{static_cast<uint64_t>(i), latency_harness::rdtsc_start()};
            while (!q.push(item)) {}
        }
    });

    std::thread consumer([&] {
        pin_thread(consumer_core);
        TimedItem item;
        for (int i = 0; i < TOTAL; ++i) {
            while (!q.pop(item)) {}
            const uint64_t recv = latency_harness::rdtsc_end();
            if (i >= WARMUP)
                latencies.push_back(recv - item.send_tsc);
        }
    });

    producer.join();
    consumer.join();

    std::sort(latencies.begin(), latencies.end());

    const double p50  = latency_harness::cycles_to_ns(latencies[MEASURED / 2]);
    const double p90  = latency_harness::cycles_to_ns(latencies[MEASURED * 90 / 100]);
    const double p99  = latency_harness::cycles_to_ns(latencies[MEASURED * 99 / 100]);
    const double p999 = latency_harness::cycles_to_ns(latencies[MEASURED * 999 / 1000]);
    const double pmax = latency_harness::cycles_to_ns(latencies.back());

    const uint64_t sum = std::accumulate(latencies.begin(), latencies.end(), uint64_t{0});
    const double mean = latency_harness::cycles_to_ns(sum / MEASURED);

    std::printf("=== SPSC One-Way Latency (%d samples, after %d warmup) ===\n",
                MEASURED, WARMUP);
    std::printf("  Mean:   %.1f ns\n", mean);
    std::printf("  p50:    %.1f ns\n", p50);
    std::printf("  p90:    %.1f ns\n", p90);
    std::printf("  p99:    %.1f ns\n", p99);
    std::printf("  p99.9:  %.1f ns\n", p999);
    std::printf("  max:    %.1f ns\n", pmax);
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// Benchmark 3: Latency under varying load
//
// Producer pushes at a controlled rate (25/50/75/100% of max throughput).
// Consumer measures pop-to-receive latency.
// Lower utilisation → lower latency (less queueing delay).
// ---------------------------------------------------------------------------
static void bench_latency_under_load(int producer_core, int consumer_core,
                                     int load_pct) {
    static constexpr int WARMUP   = 20'000;
    static constexpr int MEASURED = 200'000;
    static constexpr int TOTAL    = WARMUP + MEASURED;

    // Throttle: insert a spin loop proportional to (100 - load_pct)
    // crude but effective for a relative comparison.
    const int spin_iters = (100 - load_pct) * 10;

    SPSCQueue<TimedItem, 1024> q;
    std::vector<uint64_t> latencies;
    latencies.reserve(MEASURED);

    std::thread producer([&] {
        pin_thread(producer_core);
        for (int i = 0; i < TOTAL; ++i) {
            // Artificial throttle
            volatile int sink = 0;
            for (int s = 0; s < spin_iters; ++s) sink += s;
            (void)sink;

            TimedItem item{static_cast<uint64_t>(i), latency_harness::rdtsc_start()};
            while (!q.push(item)) {}
        }
    });

    std::thread consumer([&] {
        pin_thread(consumer_core);
        TimedItem item;
        for (int i = 0; i < TOTAL; ++i) {
            while (!q.pop(item)) {}
            const uint64_t recv = latency_harness::rdtsc_end();
            if (i >= WARMUP)
                latencies.push_back(recv - item.send_tsc);
        }
    });

    producer.join();
    consumer.join();

    std::sort(latencies.begin(), latencies.end());
    const double p50 = latency_harness::cycles_to_ns(latencies[MEASURED / 2]);
    const double p99 = latency_harness::cycles_to_ns(latencies[MEASURED * 99 / 100]);

    std::printf("  Load %3d%%: p50 = %7.1f ns  p99 = %7.1f ns\n",
                load_pct, p50, p99);
}

int main() {
    // Use cores 0 and 2 by default (sibling cores on the same socket,
    // different physical cores to avoid SMT effects).
    // Adjust these to match your CPU topology (check: lscpu --extended).
    const int PRODUCER_CORE = 0;
    const int CONSUMER_CORE = 2;

    bench_throughput(PRODUCER_CORE, CONSUMER_CORE);
    bench_latency(PRODUCER_CORE, CONSUMER_CORE);

    std::printf("=== SPSC Latency Under Load ===\n");
    for (int load : {25, 50, 75, 100})
        bench_latency_under_load(PRODUCER_CORE, CONSUMER_CORE, load);
    std::printf("\n");

    return 0;
}
