#pragma once

// ---------------------------------------------------------------------------
// memory_order_notes.hpp
//
// This header contains no executable code.  It exists to document the
// memory ordering decisions made throughout this library in one place,
// so a reviewer can cross-reference the inline comments in spsc_queue.hpp
// and mpmc_queue.hpp against a complete theoretical treatment.
//
// Target reader: an engineer who knows C++ but is new to the memory model.
// After reading this file, inline comments like "// acquire: pairs with
// consumer's release" should be immediately intelligible.
// ---------------------------------------------------------------------------

// ===========================================================================
// 1. THE C++ MEMORY MODEL IN 60 SECONDS
// ===========================================================================
//
// The C++ memory model specifies the possible orderings in which memory
// operations (reads and writes) can be observed by different threads.  The
// key insight: absent explicit synchronisation, the compiler and CPU are free
// to reorder, speculate, and cache operations in any order that preserves
// the semantics of a single thread in isolation.  Lock-free code must opt in
// to the specific cross-thread guarantees it needs — no more, no less.
//
// The six memory orders from weakest to strongest:
//
//   memory_order_relaxed
//     No ordering constraints relative to other memory operations.
//     Only guarantees atomicity of the operation itself (no torn reads/writes).
//     The cheapest option — on x86-64, atomic loads/stores with relaxed order
//     compile to plain MOV instructions.
//
//   memory_order_acquire  (loads only)
//     No loads or stores in the current thread that appear AFTER this load
//     in program order can be reordered to appear BEFORE this load.
//     Semantically: "I am about to use data that another thread published.
//     Make sure I can see all of their writes."
//
//   memory_order_release  (stores only)
//     No loads or stores in the current thread that appear BEFORE this store
//     in program order can be reordered to appear AFTER this store.
//     Semantically: "I have finished writing data.  Make it visible to anyone
//     who subsequently acquire-loads the atomic I am storing to."
//
//   memory_order_acq_rel  (read-modify-write operations, e.g. CAS)
//     Combines acquire and release semantics on a single atomic RMW.
//
//   memory_order_seq_cst
//     Imposes a single total order across ALL seq_cst operations in the
//     program, visible to all threads.  Correct but expensive:
//       - x86-64: stores become XCHG or MOV+MFENCE (~100 cycles vs ~5).
//       - ARM/POWER: even more expensive due to weaker hardware model.
//     Using seq_cst everywhere is a red flag: it signals you don't understand
//     *why* you need ordering, just that you want "more" of it.  This library
//     uses acquire/release exclusively — stronger than needed is never correct
//     in the sense of being *more* correct, only slower.
//
//   memory_order_consume
//     Weaker than acquire: ordering only for operations that carry a data
//     dependency on the loaded value.  The C++ standard permits compilers to
//     promote consume to acquire (all major compilers do this), so consume
//     provides no performance benefit and is deprecated in practice.
//     This library does not use it.

// ===========================================================================
// 2. HAPPENS-BEFORE
// ===========================================================================
//
// The happens-before relation (A hb-> B) means: if A happens-before B, then
// all effects of A are visible to B.
//
// acquire/release establishes happens-before across threads:
//
//   Thread P:  writes to X  →  tail_.store(t+1, release)
//   Thread C:  head = tail_.load(acquire)  →  reads from X
//
//   P's store happens-before C's load (release-sequence rule).
//   Therefore P's write to X happens-before C's read of X.
//   C is guaranteed to see P's write.
//
// This is the *only* cross-thread guarantee needed for SPSC correctness.
// Everything else can use relaxed ordering.

// ===========================================================================
// 3. SPSC CORRECTNESS PROOF (informal)
// ===========================================================================
//
// INVARIANT: slot i is either:
//   (a) owned by the producer for writing  (tail - head < Capacity, writer active)
//   (b) owned by the consumer for reading  (head < tail, reader active)
//   (c) free                               (neither reading nor writing)
//
// push() synchronisation chain:
//   1. Producer checks head (acquire).  Sees consumer's last release of head_.
//      If tail - head < Capacity, the slot at (tail & MASK) is free.
//   2. Producer writes buffer_[tail & MASK].
//   3. Producer stores tail+1 to tail_ (release).
//      The buffer_ write in step 2 is sequenced-before step 3, so it is
//      included in the release sequence and will be visible to any thread
//      that acquire-loads tail_.
//
// pop() synchronisation chain:
//   1. Consumer checks tail (acquire).  Pairs with producer's release of tail_.
//      This ensures the consumer sees the buffer_ write that preceded the
//      tail_ update.
//   2. Consumer reads buffer_[head & MASK].  Safe per step 1.
//   3. Consumer stores head+1 to head_ (release).
//      Pairs with the producer's acquire of head_ in the next push().
//      Signals: slot (head & MASK) is now free to overwrite.
//
// Race-free argument:
//   - buffer_[i] is written by the producer only when tail & MASK == i AND
//     the consumer's head_ acquisition showed i < tail (i.e. the slot is free).
//   - buffer_[i] is read by the consumer only when head & MASK == i AND
//     the consumer's tail_ acquisition showed i is within [head, tail).
//   - The acquire/release pairs ensure the consumer always sees the producer's
//     write before reading, and the producer always sees the consumer's read
//     before overwriting.  No data race exists.
//
// Proof that relaxed on own-index load is correct:
//   In push():  tail_ is written only by the producer.  The producer reads
//   its own write — no cross-thread synchronisation is needed.  The CPU's
//   store-to-load forwarding guarantees the producer sees its own latest value.
//   (This is true even on POWER and ARM.)

// ===========================================================================
// 4. WHY RELAXED-EVERYWHERE IS BROKEN
// ===========================================================================
//
// Suppose we replaced the acquire load of tail_ in pop() with relaxed:
//
//   const auto tail = tail_.load(std::memory_order_relaxed);  // WRONG
//   if (head == tail) return false;
//   item = buffer_[head & MASK];   // <-- can be reordered above the load!
//
// On a weakly-ordered CPU (ARM, POWER), or with an aggressive compiler, the
// CPU can speculatively execute the buffer_ read before the tail_ load
// completes.  If the producer hasn't yet written buffer_[head & MASK], the
// consumer reads garbage.  This is a data race (UB in C++).
//
// ThreadSanitizer will reliably catch this.  NEVER use relaxed-everywhere.

// ===========================================================================
// 5. WHY seq_cst EVERYWHERE IS CORRECT BUT WASTEFUL
// ===========================================================================
//
// seq_cst imposes a total order: every seq_cst store-load pair is a full
// memory barrier.  On x86-64, that means MFENCE or XCHG, which drain the
// store buffer and block the pipeline.  Measured overhead:
//   - acquire/release push+pop: ~5–10 cycles per operation
//   - seq_cst push+pop:        ~15–30 cycles per operation  (3–5x)
//
// On ARM/POWER, the gap is larger because the hardware is more weakly ordered
// and seq_cst requires explicit DMB (data memory barrier) instructions.
//
// Correct acquire/release usage is provably equivalent to seq_cst for the
// SPSC case (there is only one producer and one consumer, so there is no need
// for a total order across all threads — only the two producer-consumer pairs
// of synchronisation edges matter).

// ===========================================================================
// 6. MPMC SEQUENCE COUNTER DESIGN (Vyukov)
// ===========================================================================
//
// Each cell has a sequence counter initialised to its index.
// Invariant at steady state:
//   - cell[i].sequence == enqueue_pos means the cell is free for writing.
//   - cell[i].sequence == dequeue_pos + 1 means the cell is ready for reading.
//   - cell[i].sequence == dequeue_pos + Capacity is set after a dequeue to
//     mark the cell ready for the NEXT generation of writes.
//
// ABA prevention:
//   The sequence counter is monotonically increasing.  A cell in generation G
//   has sequence G*Capacity + i, which is globally unique across all
//   generations.  No two generations can have the same sequence value, so a
//   CAS that sees sequence == expected can only match the intended generation.
//
// Memory ordering for MPMC cells:
//   - sequence.load(acquire) in both push() and pop(): pairs with the
//     corresponding release store, ensuring data is visible before the
//     sequence update is observed.
//   - sequence.store(release) after writing data: publish the write.
//   - sequence.store(release) after reading data: free the cell for the
//     next generation.
//   - CAS on enqueue_pos_/dequeue_pos_ with relaxed success/failure ordering:
//     the CAS itself does not need to order data — that ordering is provided
//     by the subsequent sequence.store(release).

// ---------------------------------------------------------------------------
// End of memory_order_notes.hpp
// ---------------------------------------------------------------------------
