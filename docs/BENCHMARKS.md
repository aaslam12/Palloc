# Palloc Benchmark Results

Measured on Linux, 12-core Intel i5 11th gen (4.6 GHz), GCC `-O3 -flto`.
Benchmarks use Google Benchmark v1.9.1. `cycles/op` columns are raw RDTSC measured inside the benchmark loop (1000-iteration batches per sample).
Lower is better unless noted.

Single-threaded benchmarks were collected with `python build.py --stress-test --config Release --single-threaded`, which sets `PALLOC_THREADED_DEFAULT = false` and replaces every `palloc_atomic<T>` with a plain value wrapper, eliminating all `LOCK`-prefixed instructions from Palloc's alloc and free paths. jemalloc and malloc are unaffected by this flag.

Multi-threaded benchmarks were collected with `python build.py --stress-test --config Release` (the default threaded build), where every `palloc_atomic<T>` resolves to `std::atomic<T>` with full `LOCK`-prefixed RMW instructions.

---

## Single-threaded alloc+free by size class

From `jemalloc_vs_palloc`, single-threaded build. RDTSC per alloc+free pair, 1000-op batches.

| Size  | Slab (TLC) ns | Slab cycles/op | jemalloc ns | jemalloc cycles/op | malloc ns | malloc cycles/op |
|-------|--------------:|---------------:|------------:|-------------------:|----------:|-----------------:|
| 8B    | 5.45          | **14.78**      | 9.24        | 25.09              | 4.34      | 11.77            |
| 16B   | 5.47          | **14.83**      | 9.31        | 25.28              | 4.27      | 11.58            |
| 32B   | 5.43          | **14.73**      | 10.11       | 27.45              | 4.38      | 11.88            |
| 64B   | 5.44          | **14.77**      | 9.26        | 25.14              | 4.55      | 12.34            |
| 128B  | 5.46          | **14.81**      | 9.30        | 25.24              | 4.39      | 11.92            |
| 256B  | 5.43          | **14.72**      | 9.34        | 25.37              | 4.60      | 12.50            |
| 512B  | 5.45          | **14.79**      | 9.56        | 25.96              | 4.88      | 13.25            |
| 1024B | 5.46          | **14.83**      | 9.77        | 26.53              | 5.76      | 15.63            |

> Slab ~14.8 cycles/op flat, jemalloc ~25.1–26.5 cycles/op, malloc ~11.6–15.6 cycles/op.

### Why Slab achieves constant-time latency across all size classes

`slab::palloc(size)` resolves the owning pool via `size_to_index`:

```
index = INDEX_LUT[ bit_width(bit_ceil(size)) - bit_width(min_class) ]
```

Two `std::bit_width` calls plus a compile-time table lookup, with no pointer provenance walk and no tree traversal. With `PALLOC_SINGLE_THREADED`, the epoch counter read is a plain load and TLC array operations are non-atomic. The entire TLC hit path is: one plain load (epoch), one LUT lookup, one array index decrement, one pointer load. The instruction count is identical across all 10 size classes, giving the flat 14.8 cycles/op profile.

jemalloc's `free(ptr)` walks a radix tree to locate the owning arena and size class, costing 25.1–26.5 cycles/op and growing slightly at larger sizes as size-class metadata spreads across more cache lines.

malloc (glibc per-thread arena) is fastest at small sizes (11.6–12.5 cycles) because its tcache path is similarly shallow. Its cycles/op grows to 15.6 at 1024B as larger allocations trigger more coalescing work. Slab is 21% slower than malloc at 8B but 5% faster at 1024B.

---

## Fixed-size alloc+free (single-thread, 64B)

From `slab_vs_malloc_stress` and `slab_stress`, single-threaded build.

| Allocator      | ns/op | cycles/op (est.) |
|----------------|------:|-----------------:|
| **Slab (TLC)** | **4.09–4.21** | **18.8–19.4** |
| malloc         | 4.70–4.82 | 21.6–22.2       |
| jemalloc       | 9.68–9.75 | 44.5–44.9       |
| Pool           | 5.81–6.23 | 26.7–28.7       |

The tightest loop (`BM_Slab_RapidSingleSize`) reaches 4.09 ns. With `PALLOC_SINGLE_THREADED` the TLC hit path has no atomic operations: plain load of epoch, plain array decrement, plain pointer read. Pool drops from 18 ns (threaded build) to 5.8–6.2 ns because the bitmap `fetch_or`/`fetch_and` pair compiles to plain loads/stores.

---

## Linear allocation (alloc only, no free, single-thread)

From `arena_vs_malloc_stress`, single-threaded build.

| Benchmark          | Arena ns/op | malloc ns/op | Arena speedup |
|--------------------|------------:|-------------:|--------------:|
| SmallAllocs (8B)   | 0.51        | 22.57        | **44×**       |
| ResetCycle (100B)  | 0.52        | 17.88        | **34×**       |
| MixedSizes (8–64B) | 0.89        | 17.80        | **20×**       |

With `PALLOC_SINGLE_THREADED`, Arena's `alloc` compiles to a plain add on `used` with no `LOCK XADD`. This produces ~0.5 ns/op, 9× faster than the default-threaded 4.5 ns/op, and 44× faster than malloc. The speedup over the threaded build isolates the cost of `fetch_add`: removing it saves ~4 ns per allocation on this hardware.

`allocator_showdown` corroborates: Arena at 0.469 ms for 1M × 64B = 0.47 ns/op versus Pool at 2.11 ns/op. Pool's plain bitmap scan is now 2.1 ns rather than 9.5 ns, but Arena's bump pointer is still 4.5× faster.

---

## Batch alloc-then-free (256 objects, 64B)

From `jemalloc_vs_palloc` and `allocator_showdown`, single-threaded build.

| Allocator      | ns/batch | ns/op | cycles/op (est.) |
|----------------|--------:|------:|-----------------:|
| **Slab (TLC)** | **1657** | **6.47** | **29.8**      |
| malloc         | 2733    | 10.68 | 49.1             |
| jemalloc       | 3963    | 15.48 | 71.2             |

Slab leads malloc by 39% and jemalloc by 58%. With `PALLOC_SINGLE_THREADED`, TLC refill calls `alloc_batch` as a plain bitmap scan with no CAS retries, reducing the refill cost relative to the threaded build's 7.93 ns/op.

---

## Calloc (zero-initialized alloc+free)

From `allocator_showdown`, single-threaded build.

| Size  | Slab ns | Slab cycles | jemalloc ns | jemalloc cycles | malloc ns | malloc cycles |
|-------|--------:|------------:|------------:|----------------:|----------:|--------------:|
| 32B   | 7.89    | 36.3        | 11.3        | 52.0            | 8.46      | 38.9          |
| 256B  | 8.55    | 39.3        | 12.1        | 55.7            | 9.65      | 44.4          |
| 1024B | 11.8    | 54.3        | 17.3        | 79.6            | 13.7      | 63.0          |
| 4096B | 27.9    | 128.3       | 32.7        | 150.4           | 29.0      | 133.4         |

Slab leads at 32B–1024B. At 4096B all three converge as `memset` dominates.

---

## TLC hot path: alloc+free by size class (single thread)

From `slab_tlc_stress`, single-threaded build.

| Size | ns/op | cycles/op (est.) |
|-----:|------:|-----------------:|
| 8B   | 6.16  | 28.3             |
| 16B  | 6.14  | 28.2             |
| 32B  | 6.12  | 28.2             |
| 64B  | 6.32  | 29.1             |
| 128B | 6.14  | 28.2             |
| 256B | 7.66  | 35.2             |
| 512B | 7.02  | 32.3             |

The profile is mostly flat at ~6.1–6.3 ns. With `PALLOC_SINGLE_THREADED`, all TLC operations are plain loads and stores. The ~6.1 ns floor reflects the L1 load-to-use latency chain: one LUT load, one plain epoch load, one array decrement, one pointer load.

Compared to the default threaded build's 6.6 ns, single-threaded mode saves ~0.5 ns per TLC hit — the cost of replacing one `std::atomic::load(acquire)` with a plain load.

---

## TLC batch pressure (hold 129 objects, 32B)

From `slab_tlc_stress`, single-threaded build.

| Metric     | Value            |
|------------|------------------|
| Total ns   | 703 ns / 129 ops |
| ns/op      | 5.45             |
| Throughput | 367.1M ops/s     |

The forced refill calls `alloc_batch` as a plain bitmap scan with no CAS retries. Throughput improves from 313 MOps/s (threaded) to 367 MOps/s (17%), reflecting the removal of `compare_exchange_weak` retry overhead.

---

## Multi-slab TLC churn

From `slab_tlc_stress`. `NUM_CACHED_SLABS = 4`; churn test cycles through 8 slabs.

No-churn data from the single-threaded build; churn data from both builds for comparison.

| Config                              | Threads | ST ns/op | MT ns/op |
|-------------------------------------|--------:|---------:|---------:|
| No churn (≤ NUM_CACHED_SLABS)       | 1       | 6.82     | 7.05     |
| No churn                            | 8       | 10.1     | 9.93     |
| No churn                            | 16      | 12.6     | 17.4     |
| Churn (2× NUM_CACHED_SLABS)         | 1       | 76.4     | 135      |
| Churn                               | 8       | 193      | 438      |
| Churn                               | 16      | 247      | 984      |

When active slabs exceed `NUM_CACHED_SLABS`, eviction calls `cache_entry::flush()`, issuing `free_batch` on all live cached objects. The churn overhead drops from 135 ns (threaded) to 76.4 ns at 1 thread — a 43% reduction from eliminating `LOCK AND` instructions in the eviction path. At 8 threads the reduction is 56% (438→193 ns).

---

## Bitmap raw performance

From `bitmap_stress`. ST = single-threaded build, MT = default threaded build.

| Benchmark        | Threads | ST ns/op | MT ns/op |
|------------------|--------:|---------:|---------:|
| AllocFree        | 1       | 4.58     | 17.0     |
| AllocFree        | 2       | 4.65     | 17.2     |
| AllocFree        | 4       | 4.73     | 18.1     |
| AllocFree        | 8       | 4.90     | 23.2     |
| AllocFree        | 16      | 5.05     | 34.9     |
| BatchChurn (×64) | 1       | 223      | 235      |
| BatchChurn (×64) | 8       | 305      | 317      |
| BatchChurn (×64) | 16      | 394      | 399      |

The single-thread bitmap cost drops from 17.0 ns (threaded) to 4.58 ns — a 3.7× improvement. With `PALLOC_SINGLE_THREADED`, the four `LOCK`-prefixed instructions per alloc+free cycle become four plain operations. This is the direct measurement of what `PALLOC_SINGLE_THREADED` saves on the pool fallback path. The MT scaling is steep (1→16T is 2.05×) because `LOCK`-prefixed instructions serialize cache-line ownership; the ST build degrades only 1.10× because cache-line ping-pong has no serializing instruction.

---

## Multi-threaded contention (alloc+free, 32B)

From `allocator_showdown`, default threaded build.

| Threads | Slab ns | Slab cycles | jemalloc ns | jemalloc cycles | malloc ns | malloc cycles |
|--------:|--------:|------------:|------------:|----------------:|----------:|--------------:|
| 1       | 5.19    | 23.9        | 9.07        | 41.7            | 4.32      | 19.9          |
| 2       | 5.20    | 23.9        | 9.11        | 41.9            | 4.21      | 19.4          |
| 4       | 5.36    | 24.7        | 9.47        | 43.6            | 4.64      | 21.3          |
| 8       | 7.06    | 32.5        | 13.7        | 63.0            | 6.01      | 27.6          |
| 16      | 11.1    | 51.1        | 22.3        | 102.6           | 9.64      | 44.3          |

Corroborating data from `jemalloc_vs_palloc` (threads:1–8):

| Threads | Slab ns | jemalloc ns | malloc ns |
|--------:|--------:|------------:|----------:|
| 1       | 4.27    | 9.26        | 4.35      |
| 2       | 4.33    | 9.21        | 4.31      |
| 4       | 4.76    | 9.65        | 4.43      |
| 8       | 6.14    | 14.3        | 6.04      |

### Scaling analysis

At 1–4 threads, Slab's per-thread TLC absorbs all alloc and free operations without touching shared state. Each thread operates on its own `thread_local_cache` array. The only atomic on the hot path is one `acquire` load of the epoch counter, which is L1-resident and uncontended.

At 8 threads, Slab's latency rises from 5.2 to 7.1 ns (37%). Concurrent TLC misses cause multiple threads to issue `compare_exchange_weak` on the same `uint64_t` bitmap word simultaneously. The `LOCK CMPXCHG` stalls on cache-line ownership transfer under MESI, adding ~10–15 cycles per failed attempt. The thread-local word hint seeds each thread to a different starting position, reducing but not eliminating collisions.

At 16 threads, Slab reaches 11.1 ns (2.14× single-thread) vs jemalloc's 22.3 ns (2.46× single-thread). Slab degrades more gracefully: jemalloc's per-thread arena metadata is larger, causing more cache-line invalidations when multiple threads share an arena. Slab's bitmap words are 8 bytes, so up to 8 threads can contend on a single word without causing false sharing between adjacent words.

---

## Multi-threaded mixed sizes (8–4096B, round-robin per thread)

From `allocator_showdown` and `slab_thread_stress`, default threaded build.

| Threads | Slab ns | jemalloc ns | malloc ns |
|--------:|--------:|------------:|----------:|
| 1       | 8.33    | 10.9        | 5.89      |
| 2       | 8.23    | 10.8        | 5.78      |
| 4       | 8.51    | 11.2        | 6.10      |
| 8       | 11.4    | 15.3        | 8.30      |
| 16      | 19.0    | 25.5        | 13.3      |

`BM_Slab_MixedContention` from `slab_thread_stress`:

| Threads | Slab ns |
|--------:|--------:|
| 1       | 8.20    |
| 2       | 8.37    |
| 4       | 8.36    |
| 8       | 11.8    |
| 16      | 19.1    |

Slab leads jemalloc at all thread counts. malloc leads Slab because glibc's tcache requires no bitmap CAS. Slab's mixed-size penalty vs single-size (8.3 ns vs 5.2 ns at 1 thread) comes from more frequent TLC refills across 10 size classes and non-speculative `size_to_index` dispatch. At 16 threads, Slab degrades approximately linearly; malloc degrades less steeply because its per-thread tcache model partitions more state per thread.

---

## Long-lived object hold

From `slab_vs_jemalloc` and `dynamic_slab_vs_jemalloc`, default threaded build.

| Allocator | Single-thread ns/op | vs jemalloc |
|-----------|--------------------:|------------:|
| **Slab**  | **4.94**            | **1.79×**   |
| jemalloc  | 8.83                | 1.0×        |

Multi-threaded long-lived hold (500 objects, 32B):

| Threads | Slab ns | jemalloc ns | Slab vs jemalloc |
|--------:|--------:|------------:|-----------------:|
| 1       | 4.60    | 7.76        | Slab 1.69×       |
| 2       | 8.25    | 7.71        | jemalloc 1.07×   |
| 4       | 14.9    | 8.31        | jemalloc 1.79×   |
| 8       | 25.4    | 11.6        | jemalloc 2.19×   |

At 1 thread, the TLC absorbs the hold without touching the shared bitmap. At 2+ threads, each thread's 500-object hold overflows the TLC, placing 372 allocs onto the shared pool bitmap per thread. With 8 threads, 8 × 372 = 2976 concurrent bitmap slots are live simultaneously, maximizing `LOCK CMPXCHG` contention. jemalloc's per-thread arena design segregates each thread's allocations, so its 8-thread hold cost (11.6 ns) is only 1.5× its single-thread cost; Slab's is 5.5×.

---

## Realistic workload: order book simulation

56-byte Order objects (64B slab class), random add/cancel/match. 1M ops per iteration.  
`cycles/op` RDTSC-measured over the op loop only. Default threaded build.

| Allocator      | ms/iter | cycles/op |
|----------------|--------:|----------:|
| **Slab (TLC)** | 68.6    | **138.0** |
| malloc         | 51.5    | 139.6     |
| jemalloc       | 56.1    | 152.1     |
| Pool           | 56.4    | 152.4     |
> NOTE: The higher ms/iter for Slab is a benchmark artifact. Slab's construction occurs inside the Google Benchmark timed loop. RDTSC around the op loop is the correct comparison. 

Slab achieves the lowest cycles/op (138.0 vs 139.6 malloc, 152.1 jemalloc). Every alloc and free hits the TLC hot path with only one uncontended `acquire` load, keeping allocator overhead below 2% of total simulation cycles.

---

## Realistic workload: market data feed (batch processing)

Mixed message sizes: 60% quote (64B), 30% trade (128B), 10% snapshot (512B). Batch of 200 messages. Single-threaded build.

| Allocator         | ns/msg | MOps/s |
|-------------------|-------:|-------:|
| **Arena (batch)** | **13.2** | **75.6** |
| Slab (TLC)        | 21.3   | 46.9   |
| malloc            | 20.7   | 48.5   |
| jemalloc          | 25.3   | 39.7   |

Arena dominates because each batch of 200 messages uses a single plain add on `used` per allocation, then one `arena.reset()` for the entire batch. Without `LOCK XADD`, Arena's per-message cost is ~0.5 ns/alloc, making the 13.2 ns/msg figure almost entirely application logic. Arena's 1.57× advantage over malloc (vs 1.15× in the threaded build) directly reflects the removal of the atomic bump pointer.

Slab (21.3 ns/msg) and jemalloc (25.3 ns/msg) both pay per-object allocation and deallocation costs. Slab leads jemalloc by 1.19× because the TLC handles the 64B and 128B classes with a plain array pop.

---

## Realistic workload: producer-consumer (SPSC queue, 200K messages)

From `producer_consumer_sim`. Producer allocates, consumer frees. Default threaded build.

| Allocator      | ms/iter | MOps/s |
|----------------|--------:|-------:|
| malloc         | 3.40    | 58.8   |
| jemalloc       | 4.84    | 41.3   |
| **Slab (TLC)** | 7.35    | 27.2   |
| Pool           | 25.3    | 7.9    |

Slab is 2.16× slower than malloc. The TLC is per-thread: when the consumer calls `slab.free(ptr, 64)`, it accumulates freed pointers in its own TLC until it reaches 128, then flushes via `pool_view::free_batch`, issuing `LOCK AND` on shared bitmap words. Concurrently, the producer's `alloc_batch` issues `LOCK CMPXCHG` on the same words, creating write-write MESI coherence traffic. malloc's glibc remote-free list (`fastbin`) isolates producer and consumer atomic operations to separate memory locations, avoiding this conflict. Pool is slowest because every individual `free` immediately issues `LOCK AND` with no buffering.

---

## Realistic workload: fragmentation stress

50K live slots, mixed sizes 16–512B, random replacement. Default threaded build.

| Allocator    | ms/iter | ns/op | MOps/s |
|--------------|--------:|------:|-------:|
| malloc       | 3.38    | 67.6  | 14.8   |
| jemalloc     | 3.99    | 79.8  | 12.5   |
| **Slab**     | 7.03    | 140.6 | 7.1    |

Slab is 2.08× slower than malloc. The fixed power-of-two size classes cause internal fragmentation (a 17B request uses a 32B slot), reducing effective pool utilization and triggering more `virtual_commit` calls. malloc and jemalloc use variable-granularity classes with coalescing. Slab's `shrink()` is not called on the hot path.

---

## Realistic workload: Arena vs malloc (single-thread, head-to-head)

From `arena_vs_malloc_stress`, single-threaded build.

| Benchmark          | Arena ns/op | malloc ns/op | Speedup |
|--------------------|------------:|-------------:|--------:|
| SmallAllocs (8B)   | 0.51        | 22.57        | **44×** |
| ResetCycle         | 0.52        | 17.88        | **34×** |
| MixedSizes         | 0.89        | 17.80        | **20×** |

The 44× speedup is the cleanest measure of what removing `LOCK XADD` from the allocation path achieves. malloc's per-object cost is fixed regardless of atomic mode; Arena's collapses from 4.53 ns to 0.51 ns.

### Arena multi-threaded degradation

From `arena_thread_stress`, default threaded build.

| Threads | ns/op | MOps/s   |
|--------:|------:|---------:|
| 1       | 9.05  | 110.5    |
| 2       | 93.2  | 10.7     |
| 4       | 192   | 5.2      |
| 8       | 315   | 3.2      |
| 16      | 386   | 2.6      |

The Arena's `fetch_add` on `used` serializes all threads through a single `LOCK XADD`. At 2 threads, latency increases 10.3×; at 16 threads, 42.6×. The Arena is explicitly a single-owner allocator; for multi-threaded use each thread should own a private instance.

Pool degrades similarly: 18.2 ns at 1 thread, 144 ns at 2 threads (7.9×), 895 ns at 16 threads (49.2×). The two-pass scan with a thread-local hint reduces contention without eliminating it.

---

## Cache locality: slab vs malloc (50K objects, pointer traversal)

From `cache_locality_bench`, single-threaded build.

| Allocator | ms/iter | relative |
|-----------|--------:|:---------|
| **Slab**  | **2.80** | 1.0×    |
| malloc    | 3.62    | 1.29×   |

Slab is 29% faster on pointer-heavy traversal. Slab allocates from a contiguous committed virtual region, giving sequentially-allocated objects predictable stride and a tight cache footprint. For 50K × 64-byte objects (3.2 MB), the entire working set fits inside the 12MB L3 cache. malloc objects can be scattered across glibc's heap, producing a higher fraction of cold cache lines on traversal.

---

## Pool vs malloc (single-thread, head-to-head)

From `pool_vs_malloc_stress`, single-threaded build.

| Benchmark           | Pool     | malloc   | Pool advantage |
|---------------------|--------:|--------:|---------------:|
| AllocFree 64B       | 5.81 ns | 4.70 ns | malloc 1.24×   |
| AllocFree 128B      | 5.77 ns | 4.66 ns | malloc 1.24×   |
| BatchCycle (50K)    | 0.172 ms| 1.52 ms | **Pool 8.84×** |
| Exhaust 256 (5K)    | 0.017 ms| 0.234 ms| **Pool 13.8×** |

Pool's single-op latency drops from 18 ns (threaded) to 5.8 ns — a 3.1× improvement from removing the four `LOCK`-prefixed instructions per alloc+free cycle. Pool now approaches malloc's single-op latency. The batch and exhaustion advantages grow substantially: without atomic serialization, Pool's sequential bitmap scan is nearly free, making it 13.8× faster than malloc on full-exhaustion patterns.

---

## Profiling Artifacts

Perf artifacts explain where time and coherency traffic go. Regeneration instructions in [`docs/PROFILING.md`](docs/PROFILING.md).

### Flamegraphs

| Workload | Artifact |
|----------|----------|
| Slab TLC stress | [`docs/artifacts/flamegraphs/slab_tlc_stress.svg`](docs/artifacts/flamegraphs/slab_tlc_stress.svg) |
| Pool threaded stress | [`docs/artifacts/flamegraphs/pool_thread_stress.svg`](docs/artifacts/flamegraphs/pool_thread_stress.svg) |

### Perf Counter Snapshots

| Workload             | Time    | cycles  | instructions | cache misses | L1-dcache misses | load HITM | RFO HITM |
|----------------------|--------:|--------:|-------------:|-------------:|-----------------:|----------:|---------:|
| `slab_tlc_stress`    | 13.81 s | 86.79B  | 347.31B      | 0.81M        | 128.95M          | 17.48M    | 3.45M    |
| `pool_thread_stress` | 12.01 s | 171.93B | 752.00B      | 0.51M        | 7198.53M         | 3.13M     | 29.41M   |
| `pool_stress`        | 2.79 s  | 11.79B  | 41.44B       | 0.26M        | 143.22M          | 0         | 0        |

All snapshots are from the default threaded build (`PALLOC_SINGLE_THREADED=OFF`).

`pool_stress` is the single-thread baseline: near-zero HITM confirms the bitmap data structure causes no false sharing when accessed by one thread. `pool_thread_stress` hammers one shared pool from multiple threads: the 29.4M RFO HITM count reflects threads competing for exclusive ownership of bitmap cache lines via `LOCK`-prefixed writes. Note the very high L1-dcache miss count (7198M) — the multi-thread pool benchmark generates substantial cache invalidation traffic as threads fight over the same bitmap words.

`slab_tlc_stress` shows 17.5M load HITM and 3.5M RFO HITM. These come from the multi-threaded `BM_Slab_TLC_Concurrent` and `BM_Slab_MultiSlab_Churn` sub-benchmarks within the same binary; the single-thread TLC hit path itself contributes negligible HITM.

Raw output: [`docs/artifacts/perf/`](docs/artifacts/perf/)

