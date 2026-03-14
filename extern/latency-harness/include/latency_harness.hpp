#pragma once

// ---------------------------------------------------------------------------
// latency_harness.hpp — stub / fallback implementation
//
// The real latency-harness provides cycle-accurate RDTSC-based timing that is
// tightly integrated with a histogram and reporting pipeline.  This stub
// exposes the same function-level API so that benchmarks in this repo compile
// and produce useful numbers even when the submodule has not been initialised.
//
// To use the full harness:
//   git submodule update --init --recursive
//
// The stub uses RDTSCP + LFENCE for serialised timing.  On modern x86-64:
//   - LFENCE prevents instruction reordering across the fence (load fence).
//   - RDTSCP reads the time-stamp counter atomically with the processor ID,
//     serialising with respect to preceding instructions.
//
// Accuracy: suitable for measuring operations in the 100–100,000 cycle range.
// For sub-100-cycle measurements, use perf stat or dedicated hardware counters.
//
// NOTE: RDTSC is not invariant across cores unless the processor advertises
// "Invariant TSC" (IA32_MISC_ENABLE[18]).  All Intel Nehalem+ and AMD Zen+
// CPUs support this.  Pin threads to cores (pthread_setaffinity_np) before
// measuring to guarantee this assumption holds.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <chrono>    // fallback timer for non-x86 platforms

namespace latency_harness {

// ---------------------------------------------------------------------------
// rdtsc_start() — begin a timed region
//
// LFENCE serialises prior loads before reading the counter.  This prevents
// the CPU from speculatively executing the measured code before the start
// timestamp is recorded.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::uint64_t rdtsc_start() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    std::uint32_t lo, hi;
    __asm__ volatile (
        "lfence\n\t"
        "rdtsc"
        : "=a"(lo), "=d"(hi)
        :
        : "memory"
    );
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
#else
    // Non-x86 fallback: use the system monotonic clock.
    // On Apple Silicon (ARM64), use cntvct_el0 for cycle counting if needed.
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
#endif
}

// ---------------------------------------------------------------------------
// rdtsc_end() — end a timed region
//
// RDTSCP implicitly serialises with respect to preceding instructions.
// The subsequent LFENCE prevents the CPU from executing code after this
// point before the counter read completes.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::uint64_t rdtsc_end() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    std::uint32_t lo, hi, aux;
    __asm__ volatile (
        "rdtscp\n\t"
        "lfence"
        : "=a"(lo), "=d"(hi), "=c"(aux)
        :
        : "memory"
    );
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
#endif
}

// ---------------------------------------------------------------------------
// cycles_to_ns() — approximate conversion
//
// Requires knowing the TSC frequency.  We use a conservative default of
// 3 GHz (one cycle ≈ 0.333 ns).  For accurate conversion, read from
// /sys/devices/system/cpu/cpu0/tsc_freq_khz or calibrate at startup.
// ---------------------------------------------------------------------------
constexpr double kDefaultTscGhz = 3.0;

[[nodiscard]] inline double cycles_to_ns(std::uint64_t cycles,
                                         double tsc_ghz = kDefaultTscGhz) noexcept {
    return static_cast<double>(cycles) / tsc_ghz;
}

} // namespace latency_harness
