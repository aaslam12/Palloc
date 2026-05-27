## Pool and pool_view — Full Walkthrough

---

### What pool_view is

`pool_view` is a non-owning bitmap allocator. It doesn't `mmap` anything — it receives a pointer to an already-mapped memory region and carves it into a bitmap header + payload blocks. `pool` wraps it and owns the `mmap`'d memory.

---

### The bitmap

The region layout looks like this:

```
[ bitmap words (uint64_t × N) ][ padding ][ block 0 ][ block 1 ][ ... ][ block M ]
```

Each bit in the bitmap corresponds to one block. `0` = free, `1` = allocated. 64 blocks fit in one `uint64_t` word.

Example: 128 blocks of 64B → 2 bitmap words → 16 bytes of bitmap, then the payload.

On init, `memset` zeros the bitmap (all free). Trailing bits beyond `block_count` in the last word are pre-set to `1` so the scan naturally stops there without a bounds check on every bit.

---

### The hint

`m_hint` is the index of the first bitmap word that *might* have a free bit. It's an optimization — instead of scanning from word 0 every time, start from the hint.

When `alloc()` successfully claims the last bit in a word (word becomes `~0`), it advances the hint past that word. This means sequential allocations scan forward and never waste time re-checking full words.

The hint is *advisory*. In a multi-threaded context, another thread can free a block in a word *before* the hint — the hint would then point past a word with free bits. The two-pass scan handles this: pass 0 scans from hint to end, pass 1 scans from 0 to hint if pass 0 finds nothing.

---

### Alloc — step by step

```cpp
for (int pass = 0; pass < 2; ++pass)
{
    size_t hint  = m_hint.load(relaxed);
    size_t start = (pass == 0) ? hint : 0;
    ...
    for (size_t w = start; w < ...; ++w)
    {
        uint64_t word = m_bitmap[w].load(relaxed);
        while (word != ~0)  // word has at least one free bit
        {
            size_t bit = countr_zero(~word);  // find lowest 0 bit
            uint64_t new_word = word | (1 << bit);  // set that bit

            if (CAS(m_bitmap[w], word, new_word))  // atomically claim it
            {
                m_free_count.fetch_sub(1, relaxed);
                if (new_word == ~0) m_hint.store(w+1, relaxed);
                return m_memory + (block_idx << m_block_shift);
            }
            // CAS failed: another thread claimed a bit in this word first.
            // word was updated with the new value — retry the inner loop
            // with the fresh value. This is guaranteed to converge because
            // the word only ever gets more bits set (allocs), never fewer
            // (frees go to fetch_and which always succeeds immediately).
        }
    }
}
```

`countr_zero(~word)` — flip all bits, then count trailing zeros = index of the first `0` bit = index of the first free block. Single instruction on x86 (`BSF` or `TZCNT`).

---

### Free — step by step

```cpp
uint64_t mask = 1ULL << bit_idx;
uint64_t prev = m_bitmap[word_idx].fetch_and(~mask, release);
assert((prev & mask) != 0);  // double-free check
m_free_count.fetch_add(1, relaxed);
```

`fetch_and(~mask)` atomically clears the bit — no CAS loop needed because clearing a bit is always safe regardless of what other threads are doing to other bits in the same word. Two threads can free different blocks in the same word simultaneously and both `fetch_and` calls will succeed without conflict (each clears its own bit, the AND operation is commutative across disjoint bits).

`(void)prev` — `fetch_and` returns the *old* value of the word before the AND. The assert uses it to check for double-free. In release builds, the assert is removed by the preprocessor, leaving `prev` as an unused variable — the compiler warns about unused variables. `(void)prev` is a cast to void that tells the compiler "I know this is unused, suppress the warning." Zero runtime cost.

---

### Batched free (TLC flush)

When the TLC flushes 128 pointers back to the pool, instead of 128 individual `fetch_and` calls, `free_batch` does one `fetch_and` per *word*:

```cpp
// First pass: accumulate masks per word
for (void* ptr : batch) {
    size_t bit  = (ptr - m_memory) >> m_block_shift;
    size_t word = bit / 64;
    masks[word] |= (1ULL << (bit % 64));  // local, no atomic
}
// Second pass: one atomic per touched word
for (each touched word w)
    m_bitmap[w].fetch_and(~masks[w], release);
```

128 frees in one size class typically cluster in 2–4 consecutive words → 2–4 `LOCK AND` instructions instead of 128. This is the key throughput win.

---

### Growing / extending

`pool_view` doesn't grow. `pool` maps a fixed region. The "reserve first, commit later" growth described in the design doc is not yet implemented — it's Change 2. Currently `alloc()` returns `nullptr` when all blocks are exhausted, exactly as before.

---

### Before the change: mutex timeline

```
Thread A:  lock(mutex) → scan bitmap → set bit → unlock(mutex)
Thread B:  blocked waiting for mutex
Thread C:  blocked waiting for mutex
```

One thread in the pool at a time. The `pthread_mutex_lock` on Linux involves a futex syscall if the lock is contended — that's a kernel transition, hundreds of nanoseconds.

### After the change: CAS timeline

```
Thread A:  load word → compute new_word → CAS (succeeds)
Thread B:  load word → compute new_word → CAS (fails, retries with new word) → CAS (succeeds)
Thread C:  load word → compute new_word → CAS (succeeds on different word)
```

Threads on different bitmap words never interact at all. Threads on the same word spin for at most a few cycles before one wins the CAS. No kernel involvement, no sleep/wake, no queue. The `LOCK CMPXCHG` instruction on x86 takes ~20 cycles vs ~200+ cycles for a contended mutex.

---

### Memory ordering

**`memory_order_relaxed`** — no ordering guarantees relative to other memory operations. Just atomicity of the operation itself. Used for:
- `m_free_count` reads/writes — it's advisory (a hint to avoid scanning when pool is empty), not a synchronization point
- `m_hint` reads/writes — also advisory
- Loading bitmap words before CAS — the CAS itself provides the real ordering

**`memory_order_acquire`** — on success, all writes by other threads that happened before their matching `release` are visible to this thread. Used on the CAS success path — after claiming a block, the thread needs to see any data previously written into that block by whoever freed it.

**`memory_order_release`** on `fetch_and` in `free()` — all writes by this thread before the free are visible to any thread that subsequently acquires the same location. When a block is freed and another thread allocates it, the acquiring thread sees a clean block.

**`memory_order_relaxed` on CAS failure** — when the CAS fails, we just reload `word` and retry. We don't need ordering guarantees on the failure path — we're discarding the value and trying again.

The acquire/release pair on alloc CAS success / free `fetch_and` forms the happens-before relationship: free *releases*, alloc *acquires* → alloc sees all writes prior to free.

