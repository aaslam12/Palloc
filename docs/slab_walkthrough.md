# Slab Walkthrough (Top to Bottom)

This document explains how `AL::slab<Tconfig>` works today, from configuration and memory layout to allocation, freeing, reset, and teardown.

---

## 1. What `slab` Is

`slab` is a fixed-capacity allocator made of multiple fixed-size `pool_view`s (one per size class). It adds a thread-local cache (TLC) for small classes to reduce contention and reduce bitmap traffic.

At a high level:

1. A single contiguous virtual-memory region backs all pools.
2. Each size class gets a carved sub-region and one `pool_view`.
3. Cached classes use TLC first, then refill/flush in batches to/from the backing `pool_view`.
4. Non-cached classes go directly to the backing `pool_view`.

---

## 2. What `slab_config` Controls

`slab_config` provides:

- `SIZE_CLASS_CONFIG[i].byte_size` (class size)
- `SIZE_CLASS_CONFIG[i].num_blocks` (capacity in blocks for that class)
- `SIZE_CLASS_CONFIG[i].batch_size` (TLC refill/flush batch amount)
- `NUM_CACHED_CLASSES` (number of classes that use TLC)
- `VIRTUAL_MEM_PREALLOC_SIZE` (reservation size for slab’s virtual region)

### Important: Power-of-two vs sparse configs

Both statements are true:

1. **Class sizes must be powers of two** (`is_valid_config` enforces this).
2. **Dense coverage is not required** (you can skip intermediate powers of two).

Example sparse config: `{8, 64}` is valid. Requests `9..64` map to the `64B` class through `INDEX_LUT` round-up behavior. This is also covered by tests in `tests/test_slab.cpp` under sparse config cases.

---

## 3. Memory Ownership and Layout

`slab` owns one virtual region (`m_region`, `m_region_size`).

`shared_pools` is `std::array<pool_view, ...>`.  
Each entry is initialized directly with `pool_view::init_from_region(...)` over the slab-owned region.

`pool_view` never owns memory and never unmaps memory. `slab` remains the sole owner and frees the backing region in `slab::~slab()`.

---

## 4. Construction (`slab::slab()`)

Construction flow:

1. Compute raw bytes needed for all class sub-regions (`compute_total_region_size()`).
2. Round raw bytes to page size.
3. Reserve `m_region_size = max(VIRTUAL_MEM_PREALLOC_SIZE, rounded_raw_size)`.
4. Commit only `raw_size` worth of pages.
5. Carve per-class sub-regions:
   - Align cursor to class block size.
   - Call `shared_pools[i].init_from_region(cursor, sc.byte_size, sc.num_blocks)`.
   - Advance cursor by `pool_view::required_region_size(...)`.

---

## 5. TLC Data Structures

Two internal structures matter:

1. `thread_local_cache`
   - Up to 128 pointers for one size class.
   - Tracks `current` and per-class `batch_size`.

2. `cache_entry`
   - Bound to one slab instance via `owner`.
   - Contains one `thread_local_cache` for each cached size class.
   - Stores an `epoch` snapshot for lazy invalidation after reset.

Per thread:

```cpp
thread_local std::array<cache_entry, MAX_CACHED_SLABS> caches;
```

`MAX_CACHED_SLABS` is currently `4`.

---

## 6. Why There Is a Per-Thread Slot Table

A thread can touch multiple slab instances. TLC state must stay associated with the correct slab instance. The slot table allows that association with very low overhead.

Without it, the implementation would need either:

- one shared TLC per thread (incorrect across multiple slab instances), or
- a heavier dynamic structure (hash map/LRU metadata) in hot paths.

---

## 7. Slot Selection and Eviction (`get_or_create_cache_entry`)

Lookup policy:

1. Preferred slot is `slab_id % MAX_CACHED_SLABS`.
2. If preferred slot already belongs to this slab, use it (fast path).
3. Otherwise scan for:
   - an existing slot for this slab, or
   - an empty slot.
4. If an empty slot exists, claim it.
5. If all slots are occupied, evict slot `MAX_CACHED_SLABS - 1`:
   - flush its cached objects back to old owner pools,
   - rebind slot to current slab.

This is a **fixed-victim** policy, not true LRU.

---

## 8. Is `MAX_CACHED_SLABS = 4` Too Low?

It depends on how many slab instances each thread actively uses.

- If most threads repeatedly touch up to 4 slab instances, `4` is usually fine.
- If threads frequently cycle through more than 4 slab instances, eviction churn increases.

Expected churn costs in the overflow case:

- More flushes when victim slot is evicted.
- More batch refills because useful TLC state gets replaced.
- More traffic in backing `pool_view`s.

Memory tradeoff is linear in slot count. Approximate pointer storage per thread:

`MAX_CACHED_SLABS * NUM_CACHED_CLASSES * 128 * sizeof(void*)`

With default classes (`10`) and 64-bit pointers, `MAX_CACHED_SLABS=4` is about 40 KB of pointer storage per thread (plus metadata).

---

## 9. Allocation Path (`alloc(size)`)

1. Validate size.
2. Route size to class index with `size_to_index`.
3. If class is cached (`index < NUM_CACHED_CLASSES`):
   - Get/create cache entry for this slab.
   - Compare cached epoch vs slab epoch.
   - On mismatch, invalidate all TLC buckets in that entry and update epoch.
   - Try `cache.try_pop()`.
   - If empty, refill from backing `pool_view` with `alloc_batch(batch_size, ...)`, then pop.
4. If class is not cached, call `shared_pools[index].alloc()`.

`calloc(size)` calls `alloc(size)` then zeroes the class-sized block.

---

## 10. Free Path (`free(ptr, size)`)

1. Validate size.
2. Route to class index.
3. If class is cached:
   - Resolve cache entry and epoch-check as above.
   - If TLC bucket is full, flush `batch_size` pointers to backing `pool_view` and shrink `current`.
   - Push the new pointer into TLC.
4. If class is not cached, free directly to backing `pool_view`.

This is the primary fast path. Caller-provided size makes class routing fast and avoids pointer provenance lookup.

---

## 11. Unsized Free (`free_unsized(ptr)`)

`free_unsized` is slower by design:

1. Linearly scan pools and check ownership (`p.owns(ptr)`).
2. Once owner class is found, run the same cached/direct free logic.
3. Return `true` if owned; `false` otherwise.

Use when size is unavailable; prefer sized `free(ptr, size)` when possible.

---

## 12. Reset and Epoch Invalidation

`reset()`:

1. Calls `reset()` on all backing `pool_view`s.
2. Increments slab `epoch`.

TLC entries are not eagerly walked across all threads. Instead, each thread detects stale epoch on next TLC access and lazily invalidates its local cache entry.

---

## 13. Destruction and Lifetime Rules

`slab::~slab()`:

1. Invalidates this slab’s cache entry in the **current thread’s** slot table (if present).
2. Frees slab-owned region (`m_region`).

Because TLC is thread-local, the destructor cannot synchronously clean cache entries in other threads.  
Practical rule: do not destroy a slab while other threads may still access it or hold pointers tied to it.

---

## 14. Thread-Safety Summary

- `alloc`, `free`, and `free_unsized` are intended thread-safe operations.
- TLC hot path is thread-local.
- Backing `pool_view`s provide synchronized shared access.
- `reset()` is explicitly marked non-thread-safe.

When `PALLOC_SINGLE_THREADED` is enabled, `palloc_atomic` and `pool_mutex` become no-op/plain-value forms to remove synchronization overhead.

---

## 15. Tuning Checklist for `MAX_CACHED_SLABS`

If you suspect cache-slot pressure:

1. Measure active slab instances per thread in hot paths.
2. Increase `MAX_CACHED_SLABS` experimentally.
3. Compare throughput and tail latency under eviction-heavy workloads.
4. Track per-thread memory increase from additional TLC slots.

There is no universally correct value; it is workload-dependent.
