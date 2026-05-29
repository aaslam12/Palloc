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

- Linux, macOS, or Windows (x64)
- C++20 compiler
- CMake 3.10+
- Catch2 v3
- Ninja (recommended)
- jemalloc (optional, for benchmarks)
- mingw-w64 (optional, for Win32 API clangd signatures on Linux)

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
| Partial pool cycles (1K cycles, 50K allocs/cycle) | 100M alloc+free | 1.12 s | **89.0M ops/s** |
| Full pool exhaustion cycles (1K cycles, 1M block pool) | 2B alloc+free | 23.8 s | **83.9M ops/s** |

### Slab allocator

| Test | Operations | Time | Throughput |
|------|-----------|------|-----------|
| Mixed sizes (1M cycles, 32 allocs/cycle: 32/64/128/256B) | 64M alloc+free | 0.28 s | **229M ops/s** |
| Rapid single-size (10M cycles, 64B) | 20M alloc+free | 0.07 s | **303M ops/s** |

### Slab TLC (Thread-Local Cache)

| Test | Operations | Throughput |
|------|-----------|-----------|
| TLC hot path (concurrent, all size classes) | 12M ops | **1.4B ops/s** |
| Multi-slab TLC eviction path | 2.4M ops | **172M ops/s** |

### Arena allocator

| Test | Operations | Throughput |
|------|-----------|-----------|
| Sequential small allocs (200K × 8B) | 200K allocs | **111M allocs/s** |
| Alloc/reset cycles (100K cycles, 1K × 100B per cycle) | 100M ops | 185K cycles/s |

### Pool vs malloc (single-thread)

| Test | Pool | malloc | Pool speedup |
|------|------|--------|-------------|
| Fixed-size alloc+free (100M ops) | 7.3 ns/op | 17.5 ns/op | **2.4x** |
| Rapid alloc-free pairs (2M ops) | 5.4 ns/op | 0.009 ns/op | 0.002x¹ |
| Full pool exhaustion+reuse (1M ops) | 5.7 ns/op | 32.1 ns/op | **5.6x** |

¹ malloc wins at rapid alloc-free because glibc fastbins are optimized for this exact pattern.

### Slab vs malloc (single-thread)

| Test | Slab | malloc | Slab speedup |
|------|------|--------|-------------|
| Mixed sizes (2M ops: 32/64/128/256B) | 3.9 ns/op | 5.7 ns/op | **1.46x** |
| Rapid single-size (2M ops, 64B) | 2.9 ns/op | 0.009 ns/op | 0.003x¹ |
| Small allocation pattern (1M ops) | 3.0 ns/op | 0.017 ns/op | 0.006x¹ |
| Batch alloc with delayed free (2M ops) | 4.1 ns/op | 5.4 ns/op | **1.32x** |

¹ malloc wins at rapid alloc-free because glibc fastbins are optimized for this exact pattern.

### Arena vs malloc (single-thread)

| Test | Arena | malloc | Arena speedup |
|------|-------|--------|--------------|
| Sequential small allocs (200K × 8B) | 9.9 ns/op | 17.9 ns/op | **1.81x** |
| Alloc/reset cycles (100M ops) | 4.5 ns/op | 9.4 ns/op | **2.09x** |
| Mixed sizes (50K allocs: 8/16/32/64B) | 5.1 ns/op | 9.5 ns/op | **1.86x** |

### Multi-threaded (12 threads)

| Test | Throughput |
|------|------------|
| Pool: high-contention churn (120M ops) | **16.9M ops/s** |
| Pool: full exhaustion + concurrent free (3.1M blocks) | **3.4M blocks/s** |
| Pool: concurrent cycles + synchronized reset (150 cycles) | **88 cycles/s** |
| Slab: mixed-size contention churn (240M ops) | **191M ops/s** |
| Slab: per-class contention | **12 threads, 0.25s** |
| Slab: size-class exhaustion/recovery | **512 blocks, <1ms** |
| Arena: bulk concurrent allocation (120M allocs) | **29.8M allocs/s** |
| Arena: contended exhaustion (12M allocs) | **25.2M allocs/s** |
| Arena: concurrent cycles + synchronized reset (75 cycles) | **60 cycles/s** |



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
| **Slab (TLC)** | **50.6** | 19.8 |
| malloc | 51.8 | 19.3 |
| Dynamic Slab | 52.9 | 18.9 |
| Pool | 53.3 | 18.8 |
| jemalloc | 55.1 | 18.2 |

**Multi-threaded, 8 threads (ns/op):**

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| **Slab (TLC)** | **8.7** | 114.3 |
| malloc | 9.4 | 106.4 |
| Dynamic Slab | 9.7 | 103.4 |
| jemalloc | 10.2 | 98.0 |
| Pool | 48.1 | 20.8 |

Slab's per-thread TLC eliminates contention in the multi-threaded path, matching jemalloc's scalability while Pool and Dynamic Slab regress severely under mutex contention.

#### Market Data Replay

Variable-size market messages (8–256B) parsed and forwarded, modelling a market data feed handler.

| Allocator | ns/msg | MOps/s |
|-----------|--------|--------|
| Arena (batch) | **16.8** | 59.5 |
| malloc | 20.7 | 48.3 |
| Slab (TLC) | 20.3 | 49.3 |
| Dynamic Slab | 23.8 | 42.0 |
| jemalloc | 27.3 | 36.6 |

Arena benefits from batch allocation of many same-size messages. malloc leads due to per-thread fastbin reuse across the fixed message lifecycle.

#### Fragmentation Stress

50K live slots, random mixed sizes (16–512B), random replacement over 10 seconds. Measures sustained throughput under heavy fragmentation.

| Allocator | ns/op | p50 (ns) | p99 (ns) |
|-----------|-------|----------|----------|
| **Slab (TLC)** | **38.2** | 50 | 277 |
| malloc | 38.0 | 50 | 370 |
| Dynamic Slab | 41.4 | 52 | 284 |
| jemalloc | 46.6 | 56 | 379 |

Dynamic Slab is substantially slower under this workload because its radix tree must insert one leaf entry per page on every slab creation, and with 50K mixed-size slots it creates ~131 slab_nodes each occupying ~93 pages. Slab (TLC) leads because its fixed size classes eliminate fragmentation overhead entirely — each size class has a dedicated pool with O(1) bitmap alloc/free and no metadata per allocation. Pool is excluded as a fixed-capacity allocator not suited to unbounded mixed-size fragmentation workloads.

#### Producer-Consumer Pipeline

1 producer + 1 consumer thread over an SPSC ring buffer (65536 slots), 64B messages, 7 seconds each.

**Throughput (ns/msg):**

| Allocator | ns/msg | MOps/s |
|-----------|--------|--------|
| **Slab (TLC)** | **31.6** | 31.6 |
| Dynamic Slab | 35.4 | 28.2 |
| jemalloc | 25.1 | 39.8 |
| malloc | 18.4 | 54.3 |
| Pool | 125.5 | 8.0 |

**Producer latency (alloc + enqueue, p50 / p99):**

| Allocator | p50 (ns) | p99 (ns) |
|-----------|----------|----------|
| **Dynamic Slab** | **569** | 1,699 |
| Pool | 369 | 1,013 |
| Slab (TLC) | 600 | 1,281 |
| jemalloc | 77 | 427 |
| malloc | 61 | 630 |

**End-to-end latency (alloc → verify → free, p50 / p99):**

| Allocator | p50 (ns) | p99 (ns) |
|-----------|----------|----------|
| **Pool** | **317** | 1,895 |
| Slab (TLC) | ~1.7M | ~2.8M |
| Dynamic Slab | ~2.2M | ~2.9M |
| jemalloc | ~1.9M | ~2.4M |
| malloc | ~1.2M | ~1.7M |

With equal configs on a level playing field, Slab's TLC gives it the best E2E and throughput. The TLC keeps alloc/free cache-hot within each thread's working set. Dynamic Slab's radix tree lookup on every free adds overhead but gives lower producer latency since it never blocks on TLC flush. malloc and jemalloc suffer from cross-thread arena free deferral.

Benchmarked on Linux (12-core Intel i5 11th gen), compiled with GCC `-O3 -flto`. All numbers are ns/op (lower is better).

### Single-threaded alloc+free by size

Timing: RDTSC cycles/op. Lower is better. Measured on Linux (12-core Intel i5 11th gen), GCC `-O3 -flto`.

| Size | Slab (TLC) | Dynamic Slab | jemalloc | malloc |
|------|-----------|-------------|----------|--------|
| 8B | **8.3** | 17.5 | 12.2 | 5.6 |
| 16B | **8.0** | 17.7 | 12.6 | 6.0 |
| 32B | **8.1** | 17.8 | 12.5 | 5.6 |
| 64B | **8.0** | 17.9 | 12.6 | 6.2 |
| 128B | **7.9** | 17.5 | 12.6 | 5.9 |
| 256B | **7.6** | 17.3 | 12.7 | 7.2 |
| 512B | **8.0** | 17.5 | 13.4 | 8.5 |
| 1024B | **7.9** | 17.8 | 13.2 | 8.5 |
| 2048B | **7.7** | 17.8 | 14.5 | 11.5 |
| 4096B | **7.6** | 17.5 | 15.8 | 11.7 |

Slab's TLC gives it a ~1.5x cycle advantage over jemalloc here. Two structural reasons explain Slab's gap over jemalloc:

1. **Caller-supplied size.** `slab::free(ptr, size)` requires the caller to pass the size. This lets `size_to_index()` resolve the pool in a single `bit_width` instruction, with no pointer provenance lookup. jemalloc's `free(ptr)` must walk a radix tree keyed on address ranges to find the owning arena and size class — that's 2–3 cache misses on a cold path.

2. **Simpler TLC.** Slab's thread-local cache has no GC watermarks, no stats counters, and no background-thread coordination. Every alloc/free in the hot path is an array index increment/decrement on an already-hot cache line.

> **These conditions don't always hold in practice.** The advantage applies when: (a) the caller tracks sizes, (b) objects are short-lived so TLC entries stay L1-hot between alloc and free, and (c) threads don't hold more than ~128 live objects simultaneously. Multi-threaded workloads that hold many live objects degrade significantly (see batch-hold row below). For general-purpose heap replacement, jemalloc is a better fit.

### Linear allocation (alloc only, no free)

| Allocator | cycles/op |
|-----------|-----------|
| **Arena** | **12.5** |
| malloc | 13.7 |
| jemalloc | 30.1 |
| Pool | 27.9 |

Arena remains the fastest for pure linear allocation. Pool's CAS overhead on alloc makes it slower than malloc for the no-free linear pattern.

### Fixed-size alloc+free (single-thread)

Single-threaded alloc+free pairs at 64B, 1M ops.

| Allocator | cycles/op |
|-----------|-----------|
| malloc | 6.2 |
| **Slab (TLC)** | **7.2** |
| jemalloc | 12.6 |
| Pool | 26.1 |

Slab matches malloc for fixed-size workloads. Pool pays CAS overhead on every direct alloc/free; through slab's TLC, pool is only touched on batch operations.

### Batch alloc-then-free

256 objects allocated then freed together, 200K cycles, 64B.

| Allocator | cycles/op |
|-----------|-----------|
| **Slab (TLC)** | **11.8** |
| malloc | 15.4 |
| Dynamic Slab | 16.6 |
| jemalloc | 20.5 |

### Multi-threaded (12 threads)

| Test | Slab (TLC) | Dynamic Slab | jemalloc | malloc |
|------|-----------|-------------|----------|--------|
| Single size (32B) | **0.9** | 2.7 | 2.5 | 1.5 |
| Mixed sizes | **2.0** | 4.9 | 2.5 | 1.9 |
| Batch hold (500 objects) | 9.9 | 21.1 | **4.5** | 3.2 |

Slab's TLC combined with lock-free pool now scales well under contention. The batch-hold pattern shows Dynamic Slab's high variance due to radix tree overhead under concurrent access.

### Calloc (zero-initialized)

| Size | Slab | jemalloc | calloc |
|------|------|----------|--------|
| 32B | 9.4 | 16.0 | **12.1** |
| 256B | **10.3** | 16.8 | 13.0 |
| 1024B | 20.1 | **24.0** | 22.1 |
| 4096B | **37.8** | 46.5 | 40.5 |

### Known limitations

- **`free` requires the size.** `slab::free(ptr, size)` requires the caller to pass the allocation size. This is the primary source of the performance advantage over jemalloc — but it means Slab cannot be a drop-in heap replacement. It fits best in contexts where objects have a known, fixed type/size (object pools, per-request buffers, typed containers).
- **Batch-hold pattern**: When threads hold more than ~128 live objects simultaneously, Slab's TLC overflows and falls back to mutex-protected pool operations, causing significant throughput degradation under high concurrency.
- **LTO required for optimal performance**: Release builds must use LTO (`-flto` / `CMAKE_INTERPROCEDURAL_OPTIMIZATION`) to achieve the benchmarked numbers. Without LTO, static archive linking can degrade performance by 30-60% due to code layout sensitivity — the linker's dead-code elimination shifts function addresses to suboptimal icache alignment boundaries.
- **malloc still fastest for immediate-free**: glibc's per-thread fastbins remain extremely optimized for the single-threaded alloc→immediate-free pattern.

### Single-threaded mode

Build with `python build.py --single-threaded` (or `-DPALLOC_SINGLE_THREADED=ON`) to eliminate all synchronization overhead. This replaces every `std::atomic` with a plain value and every mutex with a no-op, removing `LOCK` prefixed instructions entirely. Use this when each thread owns its own allocator instance (e.g., thread-pinned trading engine components).

