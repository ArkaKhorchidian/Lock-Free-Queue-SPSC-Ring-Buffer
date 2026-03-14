#pragma once

#include <cstddef>
#include <new>

// ---------------------------------------------------------------------------
// Cache line size detection
//
// C++17 introduced std::hardware_destructive_interference_size to query the
// L1 cache line size at compile time.  GCC/Clang emit a warning if you use
// it without suppressing -Winterference-size, because the value is ABI-relevant
// and the compiler cannot guarantee it matches the runtime CPU.  For our
// purposes (padding hot-path atomics to avoid false sharing) this is fine —
// we fall back to 64 bytes, which is correct for every x86-64 and Apple M-
// series processor in production use today.
// ---------------------------------------------------------------------------

#ifdef __cpp_lib_hardware_interference_size
    inline constexpr std::size_t CACHE_LINE_SIZE =
        std::hardware_destructive_interference_size;
#else
    inline constexpr std::size_t CACHE_LINE_SIZE = 64; // x86-64 / ARM64 assumption
#endif

// ---------------------------------------------------------------------------
// CacheLineAligned<T>
//
// Wraps a value so that it occupies at least one full cache line.  Placing
// two CacheLineAligned<std::atomic<X>> members in the same struct guarantees
// they land on different cache lines, eliminating false sharing.
//
// Design notes:
//   - alignas(CACHE_LINE_SIZE) on the struct type forces the compiler to
//     align every instance on a cache-line boundary AND to pad the struct
//     size to a multiple of CACHE_LINE_SIZE.  No manual padding array needed.
//   - We do NOT add an explicit padding member, because:
//       (a) zero-size arrays are not valid in standard C++
//       (b) the compiler's alignment-induced padding is equivalent and safer
// ---------------------------------------------------------------------------

template<typename T>
struct alignas(CACHE_LINE_SIZE) CacheLineAligned {
    T value;

    // Implicit conversion to make the wrapper transparent at call sites
    constexpr operator T&()       noexcept { return value; }
    constexpr operator const T&() const noexcept { return value; }
};

static_assert(sizeof(CacheLineAligned<char>) == CACHE_LINE_SIZE,
              "CacheLineAligned must pad to exactly one cache line");
