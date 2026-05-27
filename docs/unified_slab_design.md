# Unified Slab Design

Replaces `slab` + `dynamic_slab` with a single allocator that grows on demand, making `dynamic_slab` obsolete.

---

## What already works

`slab` already implements reserve-first-commit-later:

```cpp
// slab constructor today:
void* mem = platform_mem::virtual_alloc(m_region_size);  // PROT_NONE, no physical pages
platform_mem::virtual_commit(mem, raw_size);             // commit only what num_blocks needs
```

`VIRTUAL_MEM_PREALLOC_SIZE` (default 100GB) is reserved as a single contiguous virtual range at construction. Only the bytes required by the configured `num_blocks` are backed by physical memory. The layout of sub-regions per size class is fixed at construction and never moves — pointer-to-pool routing is `(ptr - region_base) >> block_shift`, O(1), no radix tree.

---

## Change 1: Atomic bitmap words + remove `pool_mutex`

**Why:** The current bitmap (`uint64_t[]`) is non-atomic, protected by `pool_mutex`. Every alloc and free takes a mutex lock, even on the TLC flush path where 128 frees happen at once. This is a serialization point shared across all threads.

**Mechanism:** Change bitmap storage from `uint64_t[]` to `palloc_atomic<uint64_t>[]`. Alloc uses a compare-exchange loop to atomically claim a free bit. Free uses `fetch_and` to clear a bit. The mutex is removed entirely. Under `PALLOC_SINGLE_THREADED`, `palloc_atomic` compiles to plain reads/writes — zero overhead.

**Batched free optimization:** On a TLC flush of 128 pointers, instead of 128 individual atomic RMWs, accumulate a bitmask per 64-bit word locally then write one `fetch_and` per touched word:

```cpp
uint64_t masks[num_bitmap_words] = {};

for (void* ptr : batch) {
    size_t bit  = (static_cast<std::byte*>(ptr) - m_memory) >> m_block_shift;
    masks[bit / 64] |= (1ULL << (bit % 64));  // local, no atomic
}

for (size_t w : touched_words)
    bitmap[w].fetch_and(~masks[w], std::memory_order_relaxed);  // one LOCK per word
```

128 frees in one size class touch 2–4 consecutive bitmap words → 2–4 `LOCK`-prefixed instructions instead of 128. Threads operating in different regions of the bitmap never conflict.

**Status: complete.**

---

## Change 2: Dynamic commit on exhaustion

**Why:** Currently when all `num_blocks` are allocated, `alloc()` returns nullptr. With the virtual reservation covering far more space than initially committed, we can grow instead by committing the next chunk of the reserved region.

### Two-level bitmap

A flat fine bitmap (1 bit per block) across the entire virtual ceiling is impractical — for 8B blocks across 50GB, that's ~800MB of bitmap memory. Instead, a two-level structure is used:

**Coarse bitmap** (lives in `slab`, always fully committed):
- 1 bit per chunk. 0 = chunk is not committed. 1 = chunk has been committed and may have live allocations.
- Size: `(num_chunks + 63) / 64` words. For 100GB with 2MB chunks: ~50K chunks = ~6KB. Always fits in L1 cache.
- Lives in `slab` because `slab` owns the virtual reservation and controls commit/decommit. `pool_view` is non-owning — it has no business managing physical memory state.

**Per-chunk fine bitmap** (allocated per chunk, on demand):
- 1 bit per block within a chunk. Tracks which individual blocks are free or allocated.
- Allocated when a chunk is first committed (Change 2), freed when a chunk is fully decommitted (Change 3).
- Lives logically under `pool_view`'s control (it operates on it for alloc/free), but its memory is owned and allocated by `slab`.
- When a chunk is decommitted and later recommitted, a fresh fine bitmap is allocated — the OS zero-initializes it on first access, so all blocks correctly appear free with no explicit memset needed.
- `pool_view` holds an array of fine bitmap pointers, one per chunk: `palloc_atomic<uint64_t>* m_chunk_bitmaps[num_chunks]`. Null = chunk not committed.

### Chunk size

Chunk size = initial `num_blocks * block_size` for that size class. Growth is proportional to each class's initial demand. Not a fixed global size — the 8B class and 4096B class grow by different amounts.

Stored as `m_blocks_per_chunk` in `pool_view`. Set at `init_from_region` time, passed in from `slab` (which derives it from `num_blocks` in the config). Lives in `pool_view` because alloc/free need it to compute which chunk a block belongs to.

### Virtual ceiling per size class

Derived from `VIRTUAL_MEM_PREALLOC_SIZE` proportionally at compile time:

```
class_virtual_ceiling_bytes = (class_initial_bytes / total_initial_bytes) * VIRTUAL_MEM_PREALLOC_SIZE
class_reserved_blocks       = class_virtual_ceiling_bytes / block_size
```

where `class_initial_bytes = num_blocks * block_size` and `total_initial_bytes` is the sum across all classes. Classes with larger initial commits get proportionally more virtual headroom.

Example with `VIRTUAL_MEM_PREALLOC_SIZE = 100GB`:
- 8B × 512 blocks = 4096B initial (50% of total) → 50GB / 8B = ~6.7B reserved blocks
- 16B × 256 blocks = 4096B initial (50% of total) → 50GB / 16B = ~3.4B reserved blocks

Stored as `RESERVED_BLOCKS[i]` (block count) in `slab_config` — computed `consteval`. Lives in `slab_config` because it is a compile-time property of the configuration, not runtime state. `slab` reads it at construction.

### Variables

**In `pool_view`:**

| Variable | Type | Why here | What it does |
|---|---|---|---|
| `m_blocks_per_chunk` | `size_t` | `pool_view` needs it for every alloc/free to compute chunk index | Number of blocks per growth chunk = initial `num_blocks` |
| `m_committed_blocks` | `palloc_atomic<size_t>` | Alloc scans up to this bound; atomic because multiple threads may grow simultaneously | How many blocks have committed payload pages |
| `m_reserved_blocks` | `size_t` | Upper bound for growth; read-only after init so plain `size_t` | Total virtual block capacity for this pool |
| `m_chunk_bitmaps` | `palloc_atomic<uint64_t>**` | `pool_view` operates on fine bitmaps during alloc/free; needs direct access | Array of per-chunk fine bitmap pointers; null = not committed |

**In `slab`:**

| Variable | Type | Why here | What it does |
|---|---|---|---|
| `m_coarse_bitmaps[N]` | `palloc_atomic<uint64_t>*` | `slab` owns commit/decommit; coarse bitmap tracks physical page state which is slab's responsibility | Per-pool coarse bitmap; 1 bit per chunk, 1 = committed |
| `m_pool_bases[N]` | `std::byte*` | `slab` needs the base address of each pool's payload region to compute commit offsets | Start of each size class's payload sub-region |

**In `slab_config`:**

| Variable | Type | Why here | What it does |
|---|---|---|---|
| `RESERVED_BLOCKS[N]` | `constexpr size_t[N]` | Compile-time config property; derived from `VIRTUAL_MEM_PREALLOC_SIZE` and `num_blocks` ratios | Virtual block ceiling per size class |
| `VIRTUAL_MEM_PREALLOC_SIZE` | `constexpr size_t` | User-facing knob; single top-level control over virtual address space budget | Total virtual bytes reserved for the entire slab |

### Growth flow

When `pool_view::alloc()` scans up to `m_committed_blocks` and finds no free bit, it returns nullptr to `slab::alloc_internal()`. `slab` then attempts to grow that pool:

```cpp
size_t old  = pool.m_committed_blocks.load(relaxed);
size_t next = old + pool.m_blocks_per_chunk;

if (next <= pool.m_reserved_blocks) {
    if (pool.m_committed_blocks.compare_exchange_strong(old, next)) {
        // won the CAS — we are responsible for committing this chunk
        std::byte* chunk_base = m_pool_bases[i] + old * block_size;
        platform_mem::virtual_commit(chunk_base, pool.m_blocks_per_chunk * block_size);

        // allocate and zero the fine bitmap for this chunk
        size_t words = (pool.m_blocks_per_chunk + 63) / 64;
        auto* fine_bmap = static_cast<palloc_atomic<uint64_t>*>(
            platform_mem::alloc(words * sizeof(uint64_t)));
        std::memset(fine_bmap, 0, words * sizeof(uint64_t));

        // set chunk bitmap pointer and coarse bit
        size_t chunk_idx = old / pool.m_blocks_per_chunk;
        pool.m_chunk_bitmaps[chunk_idx] = fine_bmap;
        set_coarse_bit(m_coarse_bitmaps[i], chunk_idx);
    }
    // lost the CAS — another thread already grew; retry alloc
}
// retry alloc — new blocks are now available
```

`compare_exchange_strong` (not weak) is used here because growth is rare and only happens once per chunk — a spurious failure would cause us to miss the growth opportunity and return nullptr unnecessarily.

---

## Change 3: Decommit empty chunks on free

**Why:** If a workload allocates heavily then drops back to low occupancy, physical pages remain committed indefinitely. `virtual_free` returns those pages to the OS while the virtual range stays reserved and valid.

**Mechanism:** After clearing a block's bit in `pool_view::free()`, check if the entire chunk's fine bitmap is now zero. If so, signal `slab` to decommit. `slab` calls `virtual_free` on the chunk's payload, frees the fine bitmap, clears the coarse bit, and decrements `m_committed_blocks`:

```cpp
size_t chunk_idx        = block_idx / m_blocks_per_chunk;
size_t words_per_chunk  = (m_blocks_per_chunk + 63) / 64;
auto*  fine_bmap        = m_chunk_bitmaps[chunk_idx];

bool empty = true;
for (size_t w = 0; w < words_per_chunk; ++w)
    if (fine_bmap[w].load(relaxed) != 0) { empty = false; break; }

if (empty) {
    // signal slab to decommit this chunk
    // slab calls virtual_free, frees fine bitmap, clears coarse bit
}
```

The virtual address range remains `PROT_NONE` after decommit. The next alloc into that range re-triggers Change 2. The coarse bitmap + fine bitmaps together are the complete source of truth — no cursor, no separate tracking.

---

## Change 4: `VIRTUAL_MEM_PREALLOC_SIZE` becomes a config parameter

**Why:** Previously hardcoded to 100GB. Now a template parameter on `slab_config` so users can tune their virtual address space budget. Per-class virtual ceilings (`RESERVED_BLOCKS`) are derived from it automatically — users do not set per-class limits.

---

## Summary

| | Before | After |
|---|---|---|
| Alloc/free thread safety | `pool_mutex` | Lock-free atomic bitmap (`palloc_atomic`) |
| Capacity | Fixed at `num_blocks` | Starts at `num_blocks`, grows on demand within proportional virtual slice |
| Virtual ceiling per class | Fixed (`num_blocks`) | `(class_initial_bytes / total_initial_bytes) × VIRTUAL_MEM_PREALLOC_SIZE / block_size` |
| Bitmap memory | Flat array in payload region | Two-level: 6KB coarse (always live) + per-chunk fine (allocated on commit, freed on decommit) |
| Idle memory | Committed forever | Returned to OS when chunks empty (Change 3) |
| Free routing (`dynamic_slab`) | Radix tree lookup | `(ptr - base) >> block_shift` |
| TLC flush cost | 1 mutex + N bit clears | 2–4 atomic `fetch_and` |
