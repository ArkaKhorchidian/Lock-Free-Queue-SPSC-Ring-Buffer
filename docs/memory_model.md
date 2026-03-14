# Memory Model Deep Dive

A precise treatment of the C++ memory ordering decisions in this library.
This document is the proof of correctness for the implementations in
`include/spsc_queue.hpp` and `include/mpmc_queue.hpp`.

---

## 1. The C++ Memory Model

The C++ memory model (ISO/IEC 14882:2020, §6.9.2) governs how memory
operations in one thread become visible to other threads. The key insight:
**absent explicit synchronisation, the compiler and CPU are free to reorder
any two memory operations that have no data dependency between them.**

This is not a theoretical concern. Modern CPUs implement:

- **Out-of-order execution**: instructions execute when their operands are
  ready, not in program order. A load may execute before a preceding store
  if they target different addresses.
- **Store buffers**: stores are queued in a per-core buffer before being
  committed to the cache hierarchy. A load from another core may not see a
  store until it drains the store buffer — which can be hundreds of cycles.
- **Compiler reordering**: `clang -O2` and `gcc -O3` regularly reorder
  independent loads and stores to improve instruction-level parallelism.

A correct concurrent algorithm must reason about all three simultaneously.
The C++ `std::atomic` API provides the primitives to do so.

---

## 2. Memory Orders: From Weakest to Strongest

### `memory_order_relaxed`

No ordering constraints relative to other memory operations. Only guarantees
that the operation itself is atomic (no torn read/write on the variable).

```cpp
x.store(1, std::memory_order_relaxed);   // no fence; pure atomic store
int v = x.load(std::memory_order_relaxed); // no fence; pure atomic load
```

On x86-64, relaxed loads and stores compile to plain `MOV` instructions —
no additional cost over non-atomic code. On ARM, they compile to `LDR`/`STR`.

**Use when**: you only need atomicity of a single variable, not ordering
relative to other variables. Example: a statistics counter that is read
approximately.

### `memory_order_acquire` (loads only)

No memory operation that appears **after** this load in program order can be
reordered to appear **before** it. Conceptually: "I am about to use data
published by another thread. Ensure I see all their writes."

```cpp
// Consumer
const auto tail = tail_.load(std::memory_order_acquire);
// Everything below this line sees all writes that happened
// before the matching release store to tail_.
item = buffer_[head & MASK];   // safe: sees producer's write
```

On x86-64, acquire loads compile to plain `MOV` — the x86 memory model
already prevents load-load and load-store reordering by hardware. On ARM,
they compile to `LDAR` (load-acquire), which inserts a one-way barrier.

### `memory_order_release` (stores only)

No memory operation that appears **before** this store in program order can
be reordered to appear **after** it. Conceptually: "I have finished writing
data. Make it visible to anyone who subsequently acquire-loads this atomic."

```cpp
// Producer
buffer_[tail & MASK] = item;   // write data
tail_.store(tail + 1, std::memory_order_release);
// The buffer_ write is guaranteed to be visible to any thread
// that acquire-loads tail_ and sees the updated value.
```

On x86-64, release stores compile to plain `MOV` — the x86 TSO model
prevents store-store and load-store reordering. On ARM, they compile to
`STLR` (store-release).

### `memory_order_acq_rel` (read-modify-write)

Combines acquire and release on a single RMW operation (CAS, fetch-add, etc.).

### `memory_order_seq_cst`

Imposes a **single total order** on all seq_cst operations across all threads.
Every seq_cst store-load pair is a full two-way memory barrier.

On x86-64: stores become `XCHG` or `MOV` + `MFENCE`.
On ARM: stores become `STLR` + `DMB ISH` (data memory barrier).

**Cost**: ~3–5x more expensive than acquire/release on x86; significantly
more on ARM and POWER where the hardware model is weaker.

**The red flag**: using `seq_cst` everywhere signals you don't understand
_why_ you need ordering — only that you want "more" of it. Correct
acquire/release usage is provably sufficient for this library and
indistinguishable from seq_cst in terms of observable behaviour for the
access patterns used here.

### `memory_order_consume` — do not use

Weaker than acquire; ordering only for operations with a direct data
dependency on the loaded value. All major compilers (GCC, Clang, MSVC)
currently promote `consume` to `acquire` because the distinction is difficult
to implement correctly. It is effectively deprecated. This library does not
use it.

---

## 3. Happens-Before

The *happens-before* relation (A *hb→* B) means: if A happens-before B,
then all effects of A are visible when B executes.

Within a single thread, sequenced-before (program order) implies
happens-before. The interesting case is **cross-thread** happens-before,
established by acquire/release:

> If thread P performs a **release store** to atomic `X`, and thread C
> performs an **acquire load** of `X` that reads the value written by P,
> then P's store *happens-before* C's load. All stores **sequenced-before**
> P's release store are visible to C after C's acquire load.

Diagrammatically:

```
Thread P (producer)                Thread C (consumer)
─────────────────────────────────────────────────────
buffer_[tail & MASK] = item        │
    sequenced-before               │
tail_.store(t+1, release) ─────────┤ synchronises-with
                                   ▼
                          tail_.load(acquire)
                              sequenced-before
                          item = buffer_[head & MASK]
                                   │
                           C sees P's write ✓
```

This is the **only** cross-thread synchronisation edge needed for SPSC
correctness. Everything else — the producer's own view of `tail_`, the
consumer's own view of `head_` — can use relaxed ordering.

---

## 4. SPSC Correctness: The Full Argument

### Invariant

At any point in time, each slot `buffer_[i]` is in exactly one of:

1. **Producer-owned for writing**: the producer has claimed the slot
   (`tail & MASK == i`) and has not yet published it (tail_ not yet updated).
2. **Consumer-owned for reading**: the consumer has claimed the slot
   (`head & MASK == i`) and has not yet freed it (head_ not yet updated).
3. **Free**: neither thread is using the slot.

The transitions between states are guarded by the acquire/release pairs.

### push() proof

```cpp
bool push(const T& item) noexcept {
    // (1) Load own index
    const auto tail = tail_.load(std::memory_order_relaxed);
    // Correct: producer is the sole writer of tail_. No cross-thread
    // synchronisation needed to read our own latest write.

    // (2) Check if space is available
    const auto head = head_.load(std::memory_order_acquire);
    // Acquire: synchronises with consumer's release of head_ after pop().
    // After this load, we are guaranteed to see that slot (head & MASK)
    // has been fully consumed by the consumer and is free to overwrite.
    // Without acquire here, the producer could observe a stale head_,
    // believe the queue is full, and spin unnecessarily — or worse, on
    // a weakly-ordered CPU, could see head_ updated but not see the
    // consumer's read of buffer_[head & MASK], and overwrite live data.

    if (tail - head == Capacity) return false;

    // (3) Write to buffer
    buffer_[tail & MASK] = item;
    // This store is sequenced-before the release store below.

    // (4) Publish
    tail_.store(tail + 1, std::memory_order_release);
    // Release: the buffer_ write in step (3) is included in the release
    // sequence. Any consumer that acquire-loads tail_ and sees tail+1
    // is guaranteed to also see the buffer_[tail & MASK] write.
    return true;
}
```

**No data race exists** because:
- The producer writes `buffer_[tail & MASK]` only when `tail - head < Capacity`,
  i.e., the consumer's acquire of `head_` confirmed the slot is free.
- The consumer reads `buffer_[head & MASK]` only after acquiring `tail_` and
  confirming `head < tail`, i.e., the producer has published the slot.
- The acquire/release pair establishes the happens-before edge ensuring the
  consumer sees the producer's write.

### pop() proof

```cpp
bool pop(T& item) noexcept {
    // (1) Load own index
    const auto head = head_.load(std::memory_order_relaxed);
    // Correct: consumer is the sole writer of head_.

    // (2) Check if data is available
    const auto tail = tail_.load(std::memory_order_acquire);
    // Acquire: synchronises with producer's release of tail_ after push().
    // Guarantees: we see buffer_[head & MASK] as written by the producer
    // BEFORE tail_ was advanced.
    //
    // The critical reordering that acquire prevents:
    //   Without acquire, the CPU/compiler could hoist the buffer_ read
    //   (step 3) above this load. On a weakly-ordered CPU, the consumer
    //   could read buffer_[head & MASK] before the producer has written it,
    //   producing garbage or a data race.

    if (head == tail) return false;

    // (3) Read from buffer
    item = buffer_[head & MASK];
    // Safe: tail_.load(acquire) established happens-before with producer's
    // tail_.store(release), which was sequenced-after the buffer_ write.

    // (4) Free the slot
    head_.store(head + 1, std::memory_order_release);
    // Release: the producer's acquire of head_ in push() synchronises here.
    // Signals: slot (head & MASK) is now free. The producer is guaranteed
    // to see this update before it overwrites the slot.
    return true;
}
```

---

## 5. Why Relaxed-Everywhere Is Broken

Suppose we naively use `memory_order_relaxed` on every operation:

```cpp
// BROKEN — do not use
bool pop_broken(T& item) noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto tail = tail_.load(std::memory_order_relaxed);  // ← no acquire
    if (head == tail) return false;
    item = buffer_[head & MASK];  // ← can be reordered ABOVE the tail_ load
    head_.store(head + 1, std::memory_order_relaxed);         // ← no release
    return true;
}
```

**Concrete failure scenario on ARM:**

1. The consumer speculatively reads `buffer_[head & MASK]` before loading
   `tail_` (ARM allows load-load reordering).
2. The producer has not yet written to `buffer_[head & MASK]`.
3. The consumer reads garbage.

This is a **data race** (C++ UB). ThreadSanitizer will reliably detect it.

**Equally broken: no release on `head_.store`:**

1. Consumer updates `head_` with relaxed ordering.
2. Producer loads `head_` with acquire ordering (which still pairs with
   nothing — a release is needed on the other end).
3. The producer may observe the slot as occupied even though the consumer
   has finished reading it. The queue appears perpetually full.

The happens-before chain is broken in both directions. Both sides must
participate: acquire on the reader, release on the writer.

---

## 6. Why `seq_cst` Everywhere Is Correct But Wasteful

`seq_cst` provides a global total order: every `seq_cst` store-load pair is
a full two-way barrier. For the SPSC case, this is strictly stronger than
needed. The only ordering requirement is:

> producer's buffer write *hb→* consumer's buffer read

This is precisely what a single acquire/release pair provides. A total order
across all threads is irrelevant when there is only one producer and one
consumer.

**Measured overhead on x86-64 Skylake (approximate):**

| Operation       | Acquire/Release | seq_cst   | Ratio |
|-----------------|-----------------|-----------|-------|
| Atomic store    | 1 cycle (MOV)   | ~20 cycles (XCHG / MFENCE) | ~20x |
| Atomic load     | 1 cycle (MOV)   | 1 cycle (MOV) | 1x |
| Round-trip      | ~5–10 cycles    | ~15–30 cycles | ~3–5x |

On **ARM/POWER**, the gap is larger. The hardware is more weakly ordered and
seq_cst stores require explicit `DMB ISH` (ARM) or `HWSYNC` (POWER) barrier
instructions that drain the entire store buffer and stall the pipeline.

**The lesson**: use the weakest ordering that is provably correct. For SPSC,
that is acquire/release.

---

## 7. MPMC: Sequence Counter Correctness

Vyukov's MPMC queue uses per-cell sequence counters to avoid ABA without
tagged pointers. The memory ordering is slightly different from SPSC.

### ABA Problem (why sequence counters help)

Classic ABA: thread reads pointer P=A, another thread changes P to B then
back to A, first thread's CAS on P succeeds incorrectly. This happens when
the same address/value can appear in multiple logical states.

**Solution**: sequence counters are **monotonically increasing**. Cell `i`
in generation `g` has sequence `g * Capacity + i`. No two generations share
a sequence value, so a CAS that observes `sequence == expected` can only
match the intended generation — there is no aliasing across generations.

### Memory Ordering for MPMC

**In `push()`:**

```cpp
// Check: is the cell ready for our generation?
const auto seq = cell.sequence.load(std::memory_order_acquire);
// acquire: synchronises with the consumer's release store that set
// sequence = pos + Capacity after freeing this slot.
// Ensures: we see the cell is genuinely free (consumer's read is done).

// After CAS success — publish:
cell.data = item;
cell.sequence.store(pos + 1, std::memory_order_release);
// release: consumer's acquire load of sequence synchronises here.
// Guarantees: consumer sees cell.data before observing sequence = pos+1.
```

**In `pop()`:**

```cpp
// Check: has the producer written to this cell?
const auto seq = cell.sequence.load(std::memory_order_acquire);
// acquire: synchronises with producer's release store of sequence = pos+1.
// Ensures: we see cell.data as written by the producer.

// After CAS success — free the slot:
item = cell.data;
cell.sequence.store(pos + Capacity, std::memory_order_release);
// release: next producer's acquire of sequence synchronises here.
// Signals: this slot is free for generation (pos + Capacity).
```

**The CAS operations on `enqueue_pos_`/`dequeue_pos_` use `relaxed`** for
both success and failure orderings. The data visibility ordering is handled
entirely by the `cell.sequence` acquire/release pair — the position CAS only
needs to atomically claim a slot, not to order data.

---

## 8. Further Reading

1. Vyukov, D. (2010). "Bounded MPMC queue."
   http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue

2. Preshing, J. (2012). "An introduction to lock-free programming."
   https://preshing.com/20120612/an-introduction-to-lock-free-programming/

3. Preshing, J. (2012). "Acquire and release semantics."
   https://preshing.com/20120913/acquire-and-release-semantics/

4. ISO/IEC 14882:2020 (C++20 standard), §6.9.2:
   "Multi-threaded executions and data races." — the authoritative source.

5. McKenney, P.E. (2023). "Is Parallel Programming Hard, And, If So, What
   Can You Do About It?" (perfbook).
   https://kernel.org/pub/linux/kernel/people/paulmck/perfbook/perfbook.html

6. Herlihy, M. & Shavit, N. (2012). *The Art of Multiprocessor Programming.*
   — Chapter 3 (concurrent objects) and Chapter 10 (queues).

7. Boehm, H. & Adve, S. (2008). "Foundations of the C++ Concurrency Memory
   Model." PLDI 2008. — the paper that became C++11's memory model.
