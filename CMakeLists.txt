cmake_minimum_required(VERSION 3.20)
project(lockfree_spsc
    VERSION     1.0.0
    LANGUAGES   CXX
    DESCRIPTION "Lock-free SPSC ring buffer and bounded MPMC queue (C++20)"
)

# ---------------------------------------------------------------------------
# C++ standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # -std=c++20, not -std=gnu++20

# ---------------------------------------------------------------------------
# Sanitiser options
#
# TSan and ASan are mutually exclusive — they instrument the runtime
# differently and cannot coexist in the same binary.
# Build configurations:
#   cmake -B build                        # no sanitiser (release)
#   cmake -B build -DENABLE_ASAN=ON       # AddressSanitizer + UBSan
#   cmake -B build -DENABLE_TSAN=ON       # ThreadSanitizer
# ---------------------------------------------------------------------------
option(ENABLE_ASAN "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer"                                OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN are mutually exclusive.")
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
add_compile_options(-Wall -Wextra -Wpedantic)

# Suppress the GCC/Clang warning about hardware_destructive_interference_size
# being ABI-dependent.  We acknowledge this and use a fallback of 64 bytes.
# The flag exists in GCC and upstream Clang but NOT in AppleClang.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wno-interference-size HAS_WNO_INTERFERENCE_SIZE)
if(HAS_WNO_INTERFERENCE_SIZE)
    add_compile_options(-Wno-interference-size)
endif()

if(ENABLE_ASAN)
    message(STATUS "Sanitisers: AddressSanitizer + UndefinedBehaviorSanitizer")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined)
elseif(ENABLE_TSAN)
    message(STATUS "Sanitisers: ThreadSanitizer")
    # TSan requires -O1 or higher; -O0 can generate false positives.
    # Use -O1 -g for debuggable TSan builds, NOT -O3 (optimiser may obscure races).
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=thread)
else()
    message(STATUS "Sanitisers: none (release build)")
    add_compile_options(-O3 -march=native)
endif()

# ---------------------------------------------------------------------------
# Header-only library target
# ---------------------------------------------------------------------------
add_library(lockfree_spsc INTERFACE)
target_include_directories(lockfree_spsc INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(lockfree_spsc INTERFACE cxx_std_20)

# ---------------------------------------------------------------------------
# latency-harness submodule
#
# The harness provides RDTSC-based cycle-accurate timing for benchmarks.
# If the submodule has been initialised, use the real implementation.
# Otherwise, fall back to the stub in extern/latency-harness/.
#
# To initialise the real submodule:
#   git submodule update --init --recursive
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/latency-harness/CMakeLists.txt")
    add_subdirectory(extern/latency-harness)
    message(STATUS "latency-harness: using submodule")
else()
    message(STATUS "latency-harness: submodule not found — using stub")
    # The stub CMakeLists is always present
    add_subdirectory(extern/latency-harness)
endif()

# ---------------------------------------------------------------------------
# Google Test
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    ON
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ---------------------------------------------------------------------------
# Test targets
# ---------------------------------------------------------------------------
enable_testing()

function(add_lockfree_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE lockfree_spsc GTest::gtest_main)
    target_compile_options(${name} PRIVATE -pthread)
    target_link_options(${name} PRIVATE -pthread)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_lockfree_test(test_spsc_correctness test/test_spsc_correctness.cpp)
add_lockfree_test(test_mpmc_correctness test/test_mpmc_correctness.cpp)
add_lockfree_test(test_spsc_tsan        test/test_spsc_tsan.cpp)
add_lockfree_test(test_edge_cases       test/test_edge_cases.cpp)

# TSan test variant (longer iterations, run separately under TSan build)
# In CI: cmake -B build-tsan -DENABLE_TSAN=ON && ctest --test-dir build-tsan
# All tests in the test/ directory are TSan-clean by design.

# ---------------------------------------------------------------------------
# Benchmark targets (not added to ctest — run manually)
# ---------------------------------------------------------------------------
function(add_lockfree_bench name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE lockfree_spsc latency_harness)
    target_compile_options(${name} PRIVATE -pthread)
    target_link_options(${name} PRIVATE -pthread)
endfunction()

add_lockfree_bench(bench_spsc           bench/bench_spsc.cpp)
add_lockfree_bench(bench_mpmc           bench/bench_mpmc.cpp)
add_lockfree_bench(bench_false_sharing  bench/bench_false_sharing.cpp)

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
install(DIRECTORY include/ DESTINATION include)
