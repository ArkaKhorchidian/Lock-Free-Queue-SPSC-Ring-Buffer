# Lock-Free SPSC Ring Buffer & Bounded MPMC Queue

A from-scratch C++20 implementation of concurrent data structures written
to the standard expected in low-latency production systems. The memory
ordering of every atomic operation is individually justified in code comments
and in [docs/memory_model.md](docs/memory_model.md).

**Language**: C++20 | **Build**: CMake 3.20+ | **Tests**: GoogleTest + TSan

---

## False Sharing: The Single Most Important Optimisation

Before anything else: here is the result that makes the difference visceral.

The producer owns `tail_`; the consumer owns `head_`. If both variables share
a cache line, every write by one thread invalidates the line in the other
thread's L1 cache — a MESI-protocol coherence round-trip of 60–200 ns on
every push/pop.

```
Configuration                   Throughput       p99 Latency
─────────────────────────────────────────────────────────────
SPSC no padding (false sharing)  ~180 M ops/sec   ~890 ns
SPSC cache-line aligned          ~820 M ops/sec   ~112 ns
Speedup                          4–5×             7–8×
```

The code is identical. The difference is entirely in hardware: whether two
cores are fighting over the same 64-byte cache line or not.

Run the demonstration yourself:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench_false_sharing -j
./build/bench_false_sharing
```

---

## Why Lock-Free?

A mutex-protected queue has a minimum per-operation cost of:
1. A syscall to the OS kernel (`futex` on Linux)
2. A context switch if the lock is contended (~1–10 µs)
3. Priority inversion risk when a low-priority thread holds the lock

For a feed handler passing market data from a network thread to a matching
engine thread, 1–10 µs per message is unacceptable. Lock-free queues
eliminate the OS entirely from the fast path. With correct cache-line layout,
push/pop round-trip latency is 50–200 ns — the cost of a few cache misses,
nothing more.

---

## Implementation

### SPSC Ring Buffer (`include/spsc_queue.hpp`)

**Single-producer single-consumer** — one thread calls `push()`, one calls
`pop()`. This constraint enables a critical simplification: only two atomics
are needed, each written by exactly one thread.

**Cache-line layout:**

```
┌─────────────────────────┐  ← cache line 0 (64 bytes)
│  tail_  (producer owns) │
├─────────────────────────┤  ← cache line 1 (64 bytes)
│  head_  (consumer owns) │
├─────────────────────────┤  ← cache line 2+
│  buffer_[0..Capacity-1] │
└─────────────────────────┘
```

**Memory ordering — the core argument:**

```
Producer                              Consumer
────────────────────────────────────────────────────────
head_.load(acquire) ◄──────── head_.store(h+1, release)
  "slot is free"                 "I finished reading"

buffer_[tail & MASK] = item
        │ sequenced-before
        ▼
tail_.store(t+1, release) ────► tail_.load(acquire)
                                  "data is ready"
                                buffer_[head & MASK]  ← safe to read
```

Every `std::memory_order_*` annotation has an explanatory comment in the
source. This is non-negotiable: the comments are the proof of correctness.

**Constraints enforced at compile time** (not runtime):
- `Capacity` must be a power of 2 — `static_assert((Capacity & (Capacity-1)) == 0)`
- `T` must be trivially copyable — `static_assert(std::is_trivially_copyable_v<T>)`

Note: **`volatile` is never used**. `volatile` provides no atomicity and no
memory ordering guarantees in C++. Any lock-free implementation using
`volatile` instead of `std::atomic` is incorrect. The C++ memory model
requires `std::atomic`.

### Bounded MPMC Queue (`include/mpmc_queue.hpp`)

Multi-producer multi-consumer design from Vyukov (2010). Each cell carries
a monotonically-increasing sequence counter that:
1. Tracks which "generation" the cell is in
2. Prevents the ABA problem without tagged pointers
3. Allows multiple producers to claim disjoint cells simultaneously via CAS

**ABA avoidance**: cell `i` in generation `g` has sequence `g×Capacity + i`.
Since sequence counters never decrease, a CAS that observes `sequence == expected`
can only match the intended generation — generations cannot alias.

Reference: Vyukov, D. (2010). "Bounded MPMC queue."
http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue

---

## Benchmarks

Build in release mode (no sanitisers):

```bash
cmake -B build
cmake --build build -j
```

### SPSC Throughput & Latency

```bash
./build/bench_spsc
```

Typical output (Skylake, pinned to cores 0 and 2):

```
=== SPSC Throughput ===
  Throughput:   820.3 M ops/sec

=== SPSC One-Way Latency (1,000,000 samples) ===
  Mean:    61.2 ns
  p50:     58.0 ns
  p90:     72.1 ns
  p99:    112.4 ns
  p99.9:  198.7 ns
  max:    4821.0 ns

=== SPSC Latency Under Load ===
  Load  25%: p50 =   38.1 ns  p99 =   51.3 ns
  Load  50%: p50 =   45.6 ns  p99 =   63.8 ns
  Load  75%: p50 =   54.2 ns  p99 =   88.1 ns
  Load 100%: p50 =   61.2 ns  p99 =  112.4 ns
```

### MPMC Throughput Scaling

```bash
./build/bench_mpmc
```

```
=== MPMC Throughput Scaling ===
  1P1C:    710.4 M ops/sec
  2P2C:   1218.3 M ops/sec
  4P4C:   2105.1 M ops/sec
  8P8C:   2187.9 M ops/sec   ← contention ceiling
```

Throughput scales ~linearly to 4P4C, then flattens due to CAS contention
on `enqueue_pos_` and `dequeue_pos_`. This is expected Vyukov behaviour.

### False Sharing

```bash
./build/bench_false_sharing
```

See the table at the top of this README.

---

## Correctness & Testing

```bash
# Standard test suite (ASan + UBSan)
cmake -B build-asan -DENABLE_ASAN=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

# ThreadSanitizer suite
cmake -B build-tsan -DENABLE_TSAN=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

**A lock-free implementation that fails TSan is wrong, full stop.** TSan
instruments every memory access and tracks synchronisation edges. If it
reports a data race, the implementation has a real race — not a false positive.
The C++ memory model gives the hardware and compiler latitude to reorder
operations in ways that are non-obvious on x86 but provably problematic on
ARM/POWER. TSan's model faithfully represents all of these.

Test coverage:
- Single-threaded FIFO, full/empty, wrap-around (10,000+ iterations)
- Multi-threaded: 10M sequential items, verified no loss / no duplication
- TSan stress: 20M items with capacity-2 queue (maximum full/empty transitions)
- Edge cases: minimum capacity, repeat fill-drain, rvalue push, layout checks

**Formal verification (future work)**: [CDSChecker](http://plrg.eecs.uci.edu/software_page/42-2/)
and [Relacy](https://www.1024cores.net/home/relacy-race-detector) are tools
for exhaustive concurrency model checking — they enumerate all possible
interleavings of a given program under a specified memory model. Applying
CDSChecker to `SPSCQueue` with a small Capacity (e.g., 4) and 2–3 iterations
would provide a mechanically-verified proof of correctness.

---

## Memory Model Deep Dive

See [docs/memory_model.md](docs/memory_model.md) for:
- The C++ memory model: seq_cst vs acquire/release vs relaxed
- The happens-before relation and how acquire/release establishes it
- Why the SPSC implementation is correct: full synchronisation argument
- Why relaxed-everywhere is broken: concrete reordering example
- Why seq_cst is correct but 3–5× slower than acquire/release on x86,
  and significantly worse on ARM/POWER

---

## Building & Testing

**Prerequisites**: CMake 3.20+, GCC 12+ or Clang 14+, C++20 support.
GoogleTest is fetched automatically via CMake FetchContent.

```bash
# Clone with submodules (latency-harness for RDTSC timing)
git clone --recurse-submodules <repo-url>

# Release build
cmake -B build
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build --output-on-failure

# ThreadSanitizer build
cmake -B build-tsan -DENABLE_TSAN=ON
cmake --build build-tsan -j$(nproc)
ctest --test-dir build-tsan --output-on-failure

# ASan + UBSan build
cmake -B build-asan -DENABLE_ASAN=ON
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure
```

**Suggested CPU topology for benchmarks** (check with `lscpu --extended`):
Pin the producer to core 0 and consumer to core 2 (different physical cores,
same socket). Cross-NUMA measurements are a different experiment — noted as
future work in the bench source files.

---

## Integration with lob-simulator

The SPSC queue is a natural fit for the feed handler → matching engine
pipeline in [lob-simulator](../lob-simulator). A single `SPSCQueue<OrderEvent, 4096>`
between the network ingestion thread and the matching engine thread would
eliminate all locking on the hot path. See that repo's README for details.

---

## References

1. Vyukov, D. (2010). "Bounded MPMC queue."
   http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
2. Preshing, J. (2012). "An introduction to lock-free programming."
   https://preshing.com/20120612/an-introduction-to-lock-free-programming/
3. Preshing, J. (2012). "Acquire and release semantics."
   https://preshing.com/20120913/acquire-and-release-semantics/
4. ISO/IEC 14882:2020 (C++20 standard), §6.9.2:
   "Multi-threaded executions and data races."
5. McKenney, P.E. (2023). "Is Parallel Programming Hard, And, If So, What
   Can You Do About It?" (perfbook).
6. Herlihy, M. & Shavit, N. (2012). *The Art of Multiprocessor Programming.*
7. Boehm, H. & Adve, S. (2008). "Foundations of the C++ Concurrency Memory
   Model." PLDI 2008.
