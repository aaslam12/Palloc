# Palloc

A high-performance, thread-safe memory allocator library written in C++20. Implements four complementary allocator strategies — **Arena**, **Pool**, **Slab**, and **Dynamic Slab** — each optimized for specific allocation patterns. All allocators use `mmap`/`munmap` directly, bypassing the system heap entirely.

---

## Table of Contents

- [Overview](#overview)
- [Getting Started](#getting-started)
  - [Requirements](#requirements)
  - [Building](#building)
  - [Running Tests](#running-tests)
  - [Sanitizers](#sanitizers)
  - [Using as a Library](#using-as-a-library)
- [Stress Tests](#stress-tests)
  - [Pool allocator](#pool-allocator)
  - [Slab allocator](#slab-allocator)
  - [Slab TLC](#slab-tlc-thread-local-cache)
  - [Arena allocator](#arena-allocator)
  - [Pool vs malloc](#pool-vs-malloc-single-thread)
  - [Slab vs malloc](#slab-vs-malloc-single-thread)
  - [Arena vs malloc](#arena-vs-malloc-single-thread)
  - [Multi-threaded](#multi-threaded-12-threads)
  - [Realistic workloads](#realistic-workload-benchmarks)
    - [Order Book Simulation](#order-book-simulation)
    - [Market Data Replay](#market-data-replay)
    - [Fragmentation Stress](#fragmentation-stress)
    - [Producer-Consumer Pipeline](#producer-consumer-pipeline)
- [Benchmarks](#benchmarks)
  - [Single-threaded by size](#single-threaded-allocfree-by-size)
  - [Linear allocation](#linear-allocation-alloc-only-no-free)
  - [Fixed-size alloc+free](#fixed-size-allocfree-single-thread)
  - [Batch alloc-then-free](#batch-alloc-then-free)
  - [Multi-threaded](#multi-threaded-8-threads)
  - [Calloc](#calloc-zero-initialized)
  - [Known limitations](#known-limitations)

---

## Overview

| Allocator | Strategy | Thread Safety | Capacity |
|-----------|----------|---------------|----------|
| `Arena` | Linear bump allocator | Lock-free (atomic CAS) | Fixed |
| `Pool` | Bitmap allocator (via `pool_view`) | Mutex-protected | Fixed |
| `Slab` | Multi-pool with TLC | Inherited from Pool | Fixed |
| `Dynamic Slab` | Linked list of Slabs | Lock-free traversal | Unbounded |

All allocators:
- Map memory directly with `mmap` — no `malloc` or `new`
- Are validated with **ThreadSanitizer** and **AddressSanitizer**
- 0 data races, 0 memory errors across 130 test cases (238K+ assertions)
- Release builds use LTO (`-flto`) for cross-TU optimization

---

## Getting Started

### Requirements

- POSIX compliant Operating System (library uses `mmap` `munmap`)
- C++20 compiler
- CMake 3.10+
- Catch2 v3
- Ninja (recommended)
- jemalloc (optional, for benchmarks)

### Building

```bash
# compile, run tests, and run application
python build.py

python build.py --config Release

python build.py --clean

python build.py --build-only

# compiles and only runs the stress tests
python build.py --stress-test

# single-threaded build (eliminates all atomic/mutex overhead)
python build.py --single-threaded
```

### Running Tests

Tests run automatically on every debug build.

```bash
# run all
./build/Debug/tests

./build/Debug/tests "[arena]"
./build/Debug/tests "[pool]"
./build/Debug/tests "[slab]"
./build/Debug/tests "[dynamic_slab]"

# thread-safety tests
./build/Debug/tests "[thread]"
```

### Sanitizers

```bash
# detects use-after-free, buffer overflows, leaks
python build.py --asan

# detects data races and concurrency bugs
python build.py --tsan
```

Sanitizers use separate build directories (`build/Debug-asan`, `build/Debug-tsan`) to avoid conflicts. They cannot be used together.

### Using as a Library

Palloc can be installed and used in other CMake projects:

```bash
# Install user specific library
python build.py --config Release --build-only --install ~/.local

# Or install system wide (requires sudo)
python build.py --config Release --build-only --install /usr/local
```

This installs:
- Headers: `~/.local/include/`
- Library: `~/.local/lib/libpalloc.a`
- CMake package config: `~/.local/lib/cmake/palloc/`

**Using in another project:**

In your `CMakeLists.txt`:
```cmake
find_package(palloc REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE palloc::palloc)
```

Configure and build:
```bash
cmake -B build -DCMAKE_PREFIX_PATH=~/.local
cmake --build build
```

If installed system-wide, the `CMAKE_PREFIX_PATH` hint is not needed.

---

## Stress Tests

All stress tests are in `stress_tests/` and run in **Release mode** only (`-O3`). Run with:

```bash
python build.py --config Release --stress-test
```

Results on Linux (12-core Intel i5 11th gen), compiled with GCC `-O3`.

### Pool allocator

| Test | Operations | Time | Throughput |
|------|-----------|------|-----------|
| Partial pool cycles (1K cycles, 50K allocs/cycle) | 100M alloc+free | 1.12 s | **89.5M ops/s** |
| Full pool exhaustion cycles (1K cycles, 1M block pool) | 2B alloc+free | 25.5 s | **78.4M ops/s** |

### Slab allocator

| Test | Operations | Time | Throughput |
|------|-----------|------|-----------|
| Mixed sizes (1M cycles, 32 allocs/cycle: 32/64/128/256B) | 64M alloc+free | 0.45 s | **141M ops/s** |
| Rapid single-size (10M cycles, 64B) | 20M alloc+free | 0.12 s | **167M ops/s** |

### Slab TLC (Thread-Local Cache)

| Test | Operations | Throughput |
|------|-----------|-----------|
| TLC hot path (concurrent, all size classes) | 12M ops | **1.1B ops/s** |
| Multi-slab TLC eviction path | 2.4M ops | **16.7M ops/s** |

### Arena allocator

| Test | Operations | Throughput |
|------|-----------|-----------|
| Sequential small allocs (200K × 8B) | 200K allocs | **70M allocs/s** |
| Alloc/reset cycles (100K cycles, 1K × 100B per cycle) | 100M ops | 107K cycles/s |

### Pool vs malloc (single-thread)

| Test | Pool | malloc | Pool speedup |
|------|------|--------|-------------|
| Fixed-size alloc+free (100M ops) | 9.9 ns/op | 17.5 ns/op | **1.76x** |
| Rapid alloc-free pairs (2M ops) | 7.4 ns/op | 0.69 ns/op | 0.09x¹ |
| Full pool exhaustion+reuse (1M ops) | 8.2 ns/op | 34.4 ns/op | **4.2x** |

¹ malloc wins at rapid alloc-free because glibc fastbins are optimized for this exact pattern.

### Slab vs malloc (single-thread)

| Test | Slab | malloc | Slab speedup |
|------|------|--------|-------------|
| Mixed sizes (2M ops: 32/64/128/256B) | 7.0 ns/op | 8.1 ns/op | **1.16x** |
| Rapid single-size (2M ops, 64B) | 6.0 ns/op | 0.72 ns/op | 0.12x¹ |
| Small allocation pattern (1M ops) | 5.8 ns/op | 0.70 ns/op | 0.12x¹ |
| Batch alloc with delayed free (2M ops) | 7.0 ns/op | 8.5 ns/op | **1.21x** |

¹ malloc wins at rapid alloc-free because glibc fastbins are optimized for this exact pattern.

### Arena vs malloc (single-thread)

| Test | Arena | malloc | Arena speedup |
|------|-------|--------|--------------|
| Sequential small allocs (200K × 8B) | 13.9 ns/op | 18.9 ns/op | **1.36x** |
| Alloc/reset cycles (100M ops) | 9.1 ns/op | 8.8 ns/op | ~1.0x |
| Mixed sizes (50K allocs: 8/16/32/64B) | 13.5 ns/op | 19.8 ns/op | **1.46x** |

### Multi-threaded (12 threads)

| Test | Throughput |
|------|------------|
| Pool: high-contention churn (120M ops) | **7.5M ops/s** |
| Pool: full exhaustion + concurrent free (3.1M blocks) | **3.4M blocks/s** |
| Pool: concurrent cycles + synchronized reset (150 cycles) | **88 cycles/s** |
| Slab: mixed-size contention churn (240M ops) | **191M ops/s** |
| Slab: per-class contention | **12 threads, 0.25s** |
| Slab: size-class exhaustion/recovery | **512 blocks, <1ms** |
| Arena: bulk concurrent allocation (120M allocs) | **6.1M allocs/s** |
| Arena: contended exhaustion (12M allocs) | **9.8M allocs/s** |
| Arena: concurrent cycles + synchronized reset (75 cycles) | **13 cycles/s** |



### Realistic Workload Benchmarks

These tests model real-world usage patterns. Run with:

```bash
python build.py --config Release --stress-test --build-only
./build/Release/order_book_sim
./build/Release/market_data_replay
./build/Release/fragmentation_stress
./build/Release/producer_consumer_sim
```

Results on Linux (12-core Intel i5 11th gen), GCC `-O3`. Each test run 3× for stability; averages reported.

#### Order Book Simulation

Fixed-size order objects (64B) with random fill/cancel/match, modelling a limit order book.

**Single-threaded (ns/op):**

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| malloc | **70.5** | 14.2 |
| Slab (TLC) | 71.4 | 14.0 |
| Dynamic Slab | 77.3 | 12.9 |
| jemalloc | 75.4 | 13.3 |
| Pool | 79.4 | 12.6 |

**Multi-threaded, 8 threads (ns/op):**

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| **Slab (TLC)** | **10.8** | 92.6 |
| malloc | 11.3 | 88.4 |
| jemalloc | 12.2 | 81.7 |
| Dynamic Slab | 230.8 | 4.3 |
| Pool | 299.5 | 3.3 |

Slab's per-thread TLC eliminates contention in the multi-threaded path, matching jemalloc's scalability while Pool and Dynamic Slab regress severely under mutex contention.

#### Market Data Replay

Variable-size market messages (8–256B) parsed and forwarded, modelling a market data feed handler.

| Allocator | ns/msg | MOps/s |
|-----------|--------|--------|
| malloc | **23.5** | 42.6 |
| Arena (batch) | 29.6 | 33.8 |
| jemalloc | 30.9 | 32.4 |
| Slab (TLC) | 34.1 | 29.3 |
| Dynamic Slab | 38.4 | 26.0 |

Arena benefits from batch allocation of many same-size messages. malloc leads due to per-thread fastbin reuse across the fixed message lifecycle.

#### Fragmentation Stress

50K live slots, random mixed sizes (16–512B), random replacement over 10 seconds. Measures sustained throughput under heavy fragmentation.

| Allocator | ns/op | p50 (ns) | p99 (ns) |
|-----------|-------|----------|----------|
| malloc | **61.3** | 48 | 299 |
| jemalloc | 66.7 | 52 | 304 |
| Dynamic Slab | ~525 | 412 | 1849 |

Dynamic Slab is substantially slower under this workload because its radix tree must insert one leaf entry per page on every slab creation, and with 50K mixed-size slots it creates ~131 slab_nodes each occupying ~93 pages. Pool and Slab are excluded as they are fixed-capacity allocators not suited to unbounded mixed-size fragmentation workloads.

#### Producer-Consumer Pipeline

1 producer + 1 consumer thread over an SPSC ring buffer (8192 slots), 64B messages, 7 seconds each.

**Throughput (ns/msg):**

| Allocator | ns/msg | MOps/s |
|-----------|--------|--------|
| **Slab (TLC)** | **117.7** | 8.5 |
| **Dynamic Slab** | **114.6** | 8.7 |
| malloc | 123.7 | 8.1 |
| jemalloc | 129.7 | 7.7 |
| Pool | 345.3 | 2.9 |

**Producer latency (alloc + enqueue, p50 / p99):**

| Allocator | p50 (ns) | p99 (ns) |
|-----------|----------|----------|
| **Dynamic Slab** | **81** | 258 |
| Pool | 219 | 885 |
| Slab (TLC) | 395 | 619 |
| jemalloc | 545 | 1063 |
| malloc | 612 | 1334 |

**End-to-end latency (alloc → verify → free, p50 / p99):**

| Allocator | p50 (ns) | p99 (ns) |
|-----------|----------|----------|
| Slab (TLC) | **218** | 16191 |
| Dynamic Slab | 483 | 1114 |
| Pool | 465 | 325524 |
| jemalloc | ~1.06M | ~1.12M |
| malloc | ~962K | ~1.05M |

jemalloc and malloc show extreme end-to-end latency because their `free()` path crosses a thread boundary and the consumer's cache is cold relative to the producer. Slab and Dynamic Slab's contiguous mmap regions keep cross-thread free latency low.

Benchmarked on Linux (12-core Intel i5 11th gen), compiled with GCC `-O3 -flto`. All numbers are ns/op (lower is better).

### Single-threaded alloc+free by size

| Size | Slab (TLC) | Dynamic Slab | jemalloc | malloc |
|------|-----------|-------------|----------|--------|
| 8B | **3.0** | 8.8 | 5.7 | 2.3 |
| 16B | **3.1** | 8.7 | 5.7 | 2.3 |
| 32B | **3.0** | 8.7 | 5.7 | 2.3 |
| 64B | **3.0** | 8.8 | 5.8 | 2.4 |
| 128B | **3.0** | 8.8 | 5.8 | 2.5 |
| 256B | **3.0** | 8.8 | 5.9 | 2.5 |
| 512B | **3.0** | 8.8 | 6.0 | 2.8 |
| 1024B | **3.3** | 8.8 | 6.4 | 3.2 |
| 2048B | **3.0** | 8.8 | 6.9 | 4.0 |
| 4096B | **3.1** | 8.7 | 7.7 | 5.6 |

Slab's TLC gives it a ~2x advantage over jemalloc here. Dynamic Slab improved 30-45% via LTO cross-TU inlining and the 5-level radix tree optimization (page-number keying with O(1) lookup). Two structural reasons explain Slab's gap over jemalloc:

1. **Caller-supplied size.** `slab::free(ptr, size)` requires the caller to pass the size. This lets `size_to_index()` resolve the pool in a single `bit_width` instruction, with no pointer provenance lookup. jemalloc's `free(ptr)` must walk a radix tree keyed on address ranges to find the owning arena and size class — that's 2–3 cache misses on a cold path. The 2x gap is largely this lookup cost.

2. **Simpler TLC.** Slab's thread-local cache has no GC watermarks, no stats counters, and no background-thread coordination. Every alloc/free in the hot path is an array index increment/decrement on an already-hot cache line.

> **These conditions don't always hold in practice.** The 2x figure applies when: (a) the caller tracks sizes, (b) objects are short-lived so TLC entries stay L1-hot between alloc and free, and (c) threads don't hold more than ~128 live objects simultaneously. Multi-threaded workloads that hold many live objects degrade significantly (see batch-hold row below). For general-purpose heap replacement, jemalloc is a better fit.

### Linear allocation (alloc only, no free)

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| **Arena** | **4.8** | **206.9** |
| **Pool** | **6.2** | **160.8** |
| malloc | 6.2 | 160.0 |
| jemalloc | 11.7 | 85.7 |

Arena remains the fastest for pure linear allocation. Pool matches malloc for linear allocation thanks to the bitmap allocator's `__builtin_ctzll` scan with a search hint that tracks the last allocation word.

### Fixed-size alloc+free (single-thread)

Single-threaded alloc+free pairs at 64B, 1M cycles.

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| **Slab (TLC)** | **1.6** | **612.8** |
| malloc | 2.4 | 422.2 |
| Pool | 5.9 | 170.0 |
| jemalloc | 5.9 | 169.9 |

Slab remains **the fastest allocator** for fixed-size workloads, 33% faster than malloc. Pool's bitmap allocator matches jemalloc.

### Batch alloc-then-free

256 objects allocated then freed together, 200K cycles, 64B.

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| **Slab (TLC)** | **2.9** | **341.1** |
| malloc | 5.0 | 198.2 |
| Dynamic Slab | 5.6 | 177.9 |
| jemalloc | 8.3 | 121.1 |

### Multi-threaded (8 threads)

| Test | Slab (TLC) | Dynamic Slab | jemalloc | malloc |
|------|-----------|-------------|----------|--------|
| Single size (32B) | **6.0** | 7.3 | 9.1 | 7.0 |
| Mixed sizes | **6.9** | 17.9 | 10.0 | 7.0 |
| Batch hold (500 objects) | 33.1 | 171.5 | **2.5** | 1.6 |

Slab leads in single-size multi-threaded workloads. The batch-hold pattern still exposes TLC overflow behavior as expected.

### Calloc (zero-initialized)

| Size | Slab | jemalloc | calloc |
|------|------|----------|--------|
| 32B | **3.7** | 6.5 | 4.9 |
| 256B | **4.0** | 6.8 | 4.7 |
| 1024B | **5.9** | 8.8 | 6.6 |
| 4096B | **14.8** | 18.4 | 17.3 |

Slab's calloc remains consistently faster than both glibc's calloc and jemalloc across all sizes.

### Known limitations

- **`free` requires the size.** `slab::free(ptr, size)` requires the caller to pass the allocation size. This is the primary source of the performance advantage over jemalloc — but it means Slab cannot be a drop-in heap replacement. It fits best in contexts where objects have a known, fixed type/size (object pools, per-request buffers, typed containers).
- **Batch-hold pattern**: When threads hold more than ~128 live objects simultaneously, Slab's TLC overflows and falls back to mutex-protected pool operations, causing significant throughput degradation under high concurrency.
- **LTO required for optimal performance**: Release builds must use LTO (`-flto` / `CMAKE_INTERPROCEDURAL_OPTIMIZATION`) to achieve the benchmarked numbers. Without LTO, static archive linking can degrade performance by 30-60% due to code layout sensitivity — the linker's dead-code elimination shifts function addresses to suboptimal icache alignment boundaries.
- **malloc still fastest for immediate-free**: glibc's per-thread fastbins remain extremely optimized for the single-threaded alloc→immediate-free pattern.

### Single-threaded mode

Build with `python build.py --single-threaded` (or `-DPALLOC_SINGLE_THREADED=ON`) to eliminate all synchronization overhead. This replaces every `std::atomic` with a plain value and every mutex with a no-op, removing `LOCK` prefixed instructions entirely. Use this when each thread owns its own allocator instance (e.g., thread-pinned trading engine components).

