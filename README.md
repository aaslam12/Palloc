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
| **malloc** | **53.7** | 18.6 |
| **Slab (TLC)** | **54.1** | 18.4 |
| jemalloc | 57.1 | 17.5 |
| Dynamic Slab | 67.3 | 14.9 |
| Pool | 66.7 | 15.0 |

**Multi-threaded, 8 threads (ns/op):**

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| **Slab (TLC)** | **8.3** | 120.6 |
| malloc | 9.1 | 110.5 |
| jemalloc | 9.6 | 105.2 |
| Dynamic Slab | 225.5 | 4.4 |
| Pool | 287.4 | 3.5 |

Slab's per-thread TLC eliminates contention in the multi-threaded path, matching jemalloc's scalability while Pool and Dynamic Slab regress severely under mutex contention.

#### Market Data Replay

Variable-size market messages (8–256B) parsed and forwarded, modelling a market data feed handler.

| Allocator | ns/msg | MOps/s |
|-----------|--------|--------|
| malloc | **21.7** | 46.1 |
| jemalloc | 26.4 | 37.9 |
| Arena (batch) | 27.5 | 36.4 |
| Slab (TLC) | 31.8 | 31.4 |
| Dynamic Slab | 33.2 | 30.1 |

Arena benefits from batch allocation of many same-size messages. malloc leads due to per-thread fastbin reuse across the fixed message lifecycle.

#### Fragmentation Stress

50K live slots, random mixed sizes (16–512B), random replacement over 10 seconds. Measures sustained throughput under heavy fragmentation.

| Allocator | ns/op | p50 (ns) | p99 (ns) |
|-----------|-------|----------|----------|
| malloc | **36.5** | 51 | 266 |
| jemalloc | 42.5 | 53 | 267 |
| Dynamic Slab | 527 | 445 | 1708 |

Dynamic Slab is substantially slower under this workload because its radix tree must insert one leaf entry per page on every slab creation, and with 50K mixed-size slots it creates ~131 slab_nodes each occupying ~93 pages. Pool and Slab are excluded as they are fixed-capacity allocators not suited to unbounded mixed-size fragmentation workloads.

#### Producer-Consumer Pipeline

1 producer + 1 consumer thread over an SPSC ring buffer (65536 slots), 64B messages, 7 seconds each.

**Throughput (ns/msg):**

| Allocator | ns/msg | MOps/s |
|-----------|--------|--------|
| **Slab (TLC)** | **29.1** | 34.4 |
| Dynamic Slab | 37.5 | 26.7 |
| malloc | 56.6 | 17.6 |
| jemalloc | 60.1 | 16.6 |
| Pool | 327.6 | 3.1 |

**Producer latency (alloc + enqueue, p50 / p99):**

| Allocator | p50 (ns) | p99 (ns) |
|-----------|----------|----------|
| **Dynamic Slab** | **138** | 1,298 |
| Slab (TLC) | 624 | 2,182 |
| malloc | 1,147 | 4,502 |
| jemalloc | 1,185 | 3,725 |
| Pool | 930 | 4,251 |

**End-to-end latency (alloc → verify → free, p50 / p99):**

| Allocator | p50 (ns) | p99 (ns) |
|-----------|----------|----------|
| **Slab (TLC)** | **2,591** | 4,757 |
| Dynamic Slab | 4,302 | 8,963 |
| malloc | ~3.7M | ~4.2M |
| jemalloc | ~3.9M | ~5.5M |
| Pool | ~21.5M | ~24M |

With equal configs on a level playing field, Slab's TLC gives it the best E2E and throughput. The TLC keeps alloc/free cache-hot within each thread's working set. Dynamic Slab's radix tree lookup on every free adds overhead but gives lower producer latency since it never blocks on TLC flush. malloc and jemalloc suffer from cross-thread arena free deferral.

Benchmarked on Linux (12-core Intel i5 11th gen), compiled with GCC `-O3 -flto`. All numbers are ns/op (lower is better).

### Single-threaded alloc+free by size

Timing: RDTSC cycles/op. Lower is better. Measured on Linux (12-core Intel i5 11th gen), GCC `-O3 -flto`.

| Size | Slab (TLC) | Dynamic Slab | jemalloc | malloc |
|------|-----------|-------------|----------|--------|
| 8B | **8.1** | 16.9 | 12.5 | 5.5 |
| 16B | **7.9** | 16.7 | 11.7 | 5.5 |
| 32B | **7.7** | 16.7 | 12.0 | 5.7 |
| 64B | **7.6** | 16.5 | 11.5 | 6.0 |
| 128B | **7.6** | 16.4 | 11.6 | 5.8 |
| 256B | **7.5** | 16.4 | 11.8 | 6.0 |
| 512B | **9.2** | 17.0 | 12.4 | 6.6 |
| 1024B | **7.8** | 16.8 | 12.4 | 7.0 |
| 2048B | **7.6** | 16.8 | 12.9 | 7.5 |
| 4096B | **7.9** | 16.8 | 14.4 | 10.0 |

Slab's TLC gives it a ~1.5x cycle advantage over jemalloc here. Two structural reasons explain Slab's gap over jemalloc:

1. **Caller-supplied size.** `slab::free(ptr, size)` requires the caller to pass the size. This lets `size_to_index()` resolve the pool in a single `bit_width` instruction, with no pointer provenance lookup. jemalloc's `free(ptr)` must walk a radix tree keyed on address ranges to find the owning arena and size class — that's 2–3 cache misses on a cold path.

2. **Simpler TLC.** Slab's thread-local cache has no GC watermarks, no stats counters, and no background-thread coordination. Every alloc/free in the hot path is an array index increment/decrement on an already-hot cache line.

> **These conditions don't always hold in practice.** The advantage applies when: (a) the caller tracks sizes, (b) objects are short-lived so TLC entries stay L1-hot between alloc and free, and (c) threads don't hold more than ~128 live objects simultaneously. Multi-threaded workloads that hold many live objects degrade significantly (see batch-hold row below). For general-purpose heap replacement, jemalloc is a better fit.

### Linear allocation (alloc only, no free)

| Allocator | cycles/op |
|-----------|-----------|
| **Arena** | **12.2** |
| Pool | 15.1 |
| malloc | 15.6 |
| jemalloc | 30.9 |

Arena remains the fastest for pure linear allocation. Pool matches malloc for linear allocation thanks to the bitmap allocator's `__builtin_ctzll` scan with a search hint that tracks the last allocation word.

### Fixed-size alloc+free (single-thread)

Single-threaded alloc+free pairs at 64B, 1M ops.

| Allocator | cycles/op |
|-----------|-----------|
| **Slab (TLC)** | **4.1** |
| malloc | 5.7 |
| jemalloc | 12.1 |
| Pool | 14.5 |

Slab remains **the fastest allocator** for fixed-size workloads. Pool's bitmap allocator matches jemalloc.

### Batch alloc-then-free

256 objects allocated then freed together, 200K cycles, 64B.

| Allocator | cycles/op |
|-----------|-----------|
| **Slab (TLC)** | **7.6** |
| Dynamic Slab | 12.3 |
| malloc | 14.0 |
| jemalloc | 20.7 |

### Multi-threaded (12 threads)

| Test | Slab (TLC) | Dynamic Slab | jemalloc | malloc |
|------|-----------|-------------|----------|--------|
| Single size (32B) | 41.6 | 110 | 2.2 | **1.2** |
| Mixed sizes | 15.0 | 32.1 | 3.0 | **1.5** |
| Batch hold (500 objects) | 93.3 | 371.2 | 4.5 | **3.1** |

Under 12-thread contention, Slab's TLC is not enough to overcome mutex pressure when threads share a single allocator instance. malloc and jemalloc scale better here due to per-thread arenas. The batch-hold pattern exposes TLC overflow as expected.

### Calloc (zero-initialized)

| Size | Slab | jemalloc | calloc |
|------|------|----------|--------|
| 32B | 12.4 | 15.6 | **10.8** |
| 256B | **10.7** | 15.5 | 11.5 |
| 1024B | **15.0** | 20.4 | 16.2 |
| 4096B | **38.0** | 42.3 | 38.6 |

### Known limitations

- **`free` requires the size.** `slab::free(ptr, size)` requires the caller to pass the allocation size. This is the primary source of the performance advantage over jemalloc — but it means Slab cannot be a drop-in heap replacement. It fits best in contexts where objects have a known, fixed type/size (object pools, per-request buffers, typed containers).
- **Batch-hold pattern**: When threads hold more than ~128 live objects simultaneously, Slab's TLC overflows and falls back to mutex-protected pool operations, causing significant throughput degradation under high concurrency.
- **LTO required for optimal performance**: Release builds must use LTO (`-flto` / `CMAKE_INTERPROCEDURAL_OPTIMIZATION`) to achieve the benchmarked numbers. Without LTO, static archive linking can degrade performance by 30-60% due to code layout sensitivity — the linker's dead-code elimination shifts function addresses to suboptimal icache alignment boundaries.
- **malloc still fastest for immediate-free**: glibc's per-thread fastbins remain extremely optimized for the single-threaded alloc→immediate-free pattern.

### Single-threaded mode

Build with `python build.py --single-threaded` (or `-DPALLOC_SINGLE_THREADED=ON`) to eliminate all synchronization overhead. This replaces every `std::atomic` with a plain value and every mutex with a no-op, removing `LOCK` prefixed instructions entirely. Use this when each thread owns its own allocator instance (e.g., thread-pinned trading engine components).

