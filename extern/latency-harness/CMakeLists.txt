cmake_minimum_required(VERSION 3.20)
project(latency_harness CXX)

# Stub CMakeLists for the latency-harness submodule.
# The real implementation lives at:
#   https://github.com/[username]/latency-harness
#
# To initialise after cloning this repo:
#   git submodule update --init --recursive
#
# Until the submodule is populated, this stub provides an INTERFACE target
# with a fallback inline RDTSC implementation so benchmarks compile and run
# without the full harness.

add_library(latency_harness INTERFACE)
target_include_directories(latency_harness INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(latency_harness INTERFACE cxx_std_20)
