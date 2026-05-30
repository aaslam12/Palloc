# Palloc Benchmark Results

Measured on Linux, 12-core Intel i5 11th gen, GCC `-O3 -flto`.  
All numbers are the average of 3 runs. Lower is better unless noted.

---

## Single-threaded alloc+free by size class

RDTSC cycles/op. 1M ops per size.

| Size  | Slab (TLC) | jemalloc | malloc |
|-------|----------:|--------:|------:|
| 8B    | 8.1       | 12.6    | 6.2   |
| 16B   | 8.2       | 13.1    | 6.2   |
| 32B   | 8.7       | 13.3    | 6.4   |
| 64B   | 8.6       | 14.3    | 6.3   |
| 128B  | 8.7       | 13.9    | 6.8   |
| 256B  | 8.8       | 13.9    | 7.6   |
| 512B  | 8.8       | 14.1    | 8.1   |
| 1024B | 8.6       | 14.3    | 10.1  |
| 2048B | 8.6       | 15.0    | 10.9  |
| 4096B | 8.6       | 16.2    | 12.9  |

Slab's TLC gives it a ~1.6x cycle advantage over jemalloc across all size classes (8.1–8.8 vs 12.6–16.2 cycles/op).

---

## Fixed-size alloc+free (single-thread, 64B, 1M ops)

RDTSC cycles/op.

| Allocator      | cycles/op |
|----------------|----------:|
| **Slab (TLC)** | **4.3**   |
| malloc         | 6.1       |
| jemalloc       | 13.5      |
| Pool           | 26.4      |

---

## Linear allocation (alloc only, no free, 64B, 1M ops)

RDTSC cycles/op.

| Allocator  | cycles/op |
|------------|----------:|
| **Arena**  | **13.4**  |
| malloc     | 14.1      |
| Pool       | 29.1      |
| jemalloc   | 30.8      |

Arena is the fastest for pure linear allocation. Pool's CAS overhead on alloc makes it slower than malloc for the no-free linear pattern.

---

## Multi-threaded contention (12 threads, RDTSC cycles/op)

### Single size, 32B (500K iters/thread)

| Allocator      | cycles/op |
|----------------|----------:|
| **Slab (TLC)** | **1.0**   |
| malloc         | 1.2       |
| jemalloc       | 3.1       |

### Mixed sizes (300K iters/thread)

| Allocator  | cycles/op |
|------------|----------:|
| malloc     | 2.3       |
| **Slab (TLC)** | **2.5** |
| jemalloc   | 3.2       |

### Batch-hold 500 live objects (100 cycles/thread)

| Allocator  | cycles/op |
|------------|----------:|
| malloc     | 3.9       |
| **jemalloc** | **5.2** |
| Slab (TLC) | 8.9       |

Once threads hold more than ~128 live objects simultaneously, the TLC overflows and refills hit contended CAS on the shared pool bitmap. jemalloc's per-arena partitioning wins here.

---

## TLC hot path throughput (12 threads, all size classes)

12M ops across 12 threads, each thread cycling through all 10 size classes.

| Metric         | Value         |
|----------------|---------------|
| Total ops      | 12,000,000    |
| **Throughput** | **2.2B ops/s** |

---

## Realistic workload: 8-thread order book simulation

56-byte Order objects (allocated from a 64B slab class), random fill/cancel/execute/modify. 7 seconds per allocator.

| Allocator      | ns/op | MOps/s |
|----------------|------:|-------:|
| **Slab (TLC)** | **8.7** | **114.9** |
| malloc         | 9.3   | 107.5  |
| jemalloc       | 10.0  | 100.0  |
| Pool           | 66.9  | 14.9   |

Latency percentiles (ns):

| Allocator      | p50  | p90  | p99  | p99.9 | mean  |
|----------------|-----:|-----:|-----:|------:|------:|
| **Slab (TLC)** | 149  | 322  | 1724 | 3452  | 238.8 |
| malloc         | 151  | 334  | 1801 | 3510  | 244.8 |
| jemalloc       | 167  | 374  | 1886 | 3506  | 261.3 |
| Pool           | 1146 | 2681 | 7380 | 12517 | 1533.1|

---

## Realistic workload: market data feed (batch processing)

Mixed message sizes: 60% quote (64B), 30% trade (128B), 10% snapshot (512B). Batch of 200 messages, 7 seconds per allocator.

| Allocator         | ns/msg | MOps/s |
|-------------------|-------:|-------:|
| **Arena (batch)** | **16.1** | **62.1** |
| Slab (TLC)        | 20.3   | 49.3   |
| malloc            | 20.4   | 49.0   |
| jemalloc          | 26.4   | 37.9   |

Arena leads because it batch-allocates all messages in a single bump-pointer pass with no per-object metadata.

---

## Realistic workload: fragmentation stress

50,000 live slots, mixed sizes 16–512B, random replacement. 10 seconds per allocator.

| Allocator      | ns/op | MOps/s | RSS (MB) |
|----------------|------:|-------:|---------:|
| malloc         | 38.5  | 26.0   | 16       |
| **Slab (TLC)** | **39.4** | **25.4** | 22    |
| jemalloc       | 47.7  | 21.0   | 16       |

Slab uses more RSS than jemalloc here because its fixed size classes round up allocations (e.g., a 300B object uses a 512B slot), trading memory efficiency for O(1) bitmap alloc/free.

---

## Batch alloc-then-free (256 objects × 200K cycles, 64B)

RDTSC cycles/op.

| Allocator      | cycles/op |
|----------------|----------:|
| **Slab (TLC)** | **11.7**  |
| malloc         | 15.9      |
| jemalloc       | 21.4      |

---

## Calloc (zero-initialized alloc+free, 1M ops)

RDTSC cycles/op.

| Size  | Slab | jemalloc | glibc |
|-------|-----:|---------:|------:|
| 32B   | 9.6  | 16.1     | 12.3  |
| 256B  | 11.0 | 17.9     | 13.5  |
| 1024B | 18.0 | 24.5     | 20.9  |
| 4096B | 41.8 | 46.6     | 41.7  |
