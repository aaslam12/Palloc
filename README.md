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
- [Benchmarks](#benchmarks)
  - [Single-threaded by size](#single-threaded-allocfree-by-size)
  - [Linear allocation](#linear-allocation-alloc-only-no-free)
  - [Fixed-size alloc+free](#fixed-size-allocfree-single-thread)
  - [Batch alloc-then-free](#batch-alloc-then-free)
  - [Multi-threaded](#multi-threaded-8-threads)
  - [Calloc](#calloc-zero-initialized)
  - [Known limitations](#known-limitations)
- [Architecture](#architecture)
  - [Thread-Local Cache](#thread-local-cache-tlc)

---

## Overview

| Allocator | Strategy | Thread Safety | Capacity |
|-----------|----------|---------------|----------|
| `Arena` | Linear bump allocator | Lock-free (atomic CAS) | Fixed |
| `Pool` | Free-list allocator | Mutex-protected | Fixed |
| `Slab` | Multi-pool with TLC | Inherited from Pool | Fixed |
| `Dynamic Slab` | Linked list of Slabs | Lock-free traversal | Unbounded |

All allocators:
- Map memory directly with `mmap` — no `malloc` or `new`
- Are validated with **ThreadSanitizer** and **AddressSanitizer**
- 0 data races, 0 memory errors across 88 tests + 40 thread-safety tests

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



Benchmarked on Linux (12-core Intel i5 11th gen), compiled with GCC `-O3`. All numbers are ns/op (lower is better).

### Single-threaded alloc+free by size

| Size | Slab (TLC) | Dynamic Slab | jemalloc | malloc |
|------|-----------|-------------|----------|--------|
| 8B | **2.7** | 4.0 | 5.8 | 2.4 |
| 16B | **2.7** | 4.4 | 5.7 | 2.4 |
| 32B | **2.6** | 5.1 | 5.7 | 2.4 |
| 64B | **2.6** | 5.7 | 5.7 | 2.4 |
| 128B | **2.6** | 6.2 | 5.8 | 2.5 |
| 256B | **2.6** | 6.8 | 5.8 | 2.7 |
| 512B | **2.6** | 7.4 | 6.0 | 2.8 |
| 1024B | **2.5** | 8.0 | 6.2 | 3.2 |
| 2048B | **2.6** | 8.6 | 6.8 | 4.2 |
| 4096B | **2.5** | 9.1 | 7.7 | 5.9 |

Slab's TLC gives it a ~2x advantage over jemalloc here. Two structural reasons explain the gap — both tied to the benchmark conditions:

1. **Caller-supplied size.** `slab::free(ptr, size)` requires the caller to pass the size. This lets `size_to_index()` resolve the pool in a single `bit_width` instruction, with no pointer provenance lookup. jemalloc's `free(ptr)` must walk a radix tree keyed on address ranges to find the owning arena and size class — that's 2–3 cache misses on a cold path. The 2x gap is largely this lookup cost.

2. **Simpler TLC.** Slab's thread-local cache has no GC watermarks, no stats counters, and no background-thread coordination. Every alloc/free in the hot path is an array index increment/decrement on an already-hot cache line.

> **These conditions don't always hold in practice.** The 2x figure applies when: (a) the caller tracks sizes, (b) objects are short-lived so TLC entries stay L1-hot between alloc and free, and (c) threads don't hold more than ~128 live objects simultaneously. Multi-threaded workloads that hold many live objects degrade significantly (see batch-hold row below). For general-purpose heap replacement, jemalloc is a better fit.

### Linear allocation (alloc only, no free)

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| **Arena** | **4.8** | **208.0** |
| malloc | 6.5 | 154.0 |
| Pool | 9.5 | 104.9 |
| jemalloc | 11.7 | 85.3 |

Arena is now **the fastest** for pure linear allocation, 2x faster than the previous version and 35% faster than malloc. The template refactoring enables better compile-time optimizations of the atomic fetch_add path. Pool and jemalloc remain competitive for general-purpose allocation.

### Fixed-size alloc+free (single-thread)

Single-threaded alloc+free pairs at 64B, 1M cycles.

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| **Slab (TLC)** | **1.7** | **604.3** |
| malloc | 2.4 | 410.7 |
| jemalloc | 5.7 | 174.3 |
| Pool | 7.7 | 129.4 |

Slab is now **the fastest allocator** for fixed-size workloads, 50% faster than the previous version and 41% faster than malloc.

### Batch alloc-then-free

256 objects allocated then freed together, 200K cycles, 64B.

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| **Slab (TLC)** | **5.2** | **193.1** |
| malloc | 5.9 | 168.8 |
| Dynamic Slab | 8.7 | 115.6 |
| jemalloc | 9.2 | 108.8 |

Slab's TLC handles delayed-free patterns faster than all competitors. The 10% improvement over the previous version comes from better TLC inlining via template specialization.

### Multi-threaded (8 threads)

| Test | Slab (TLC) | Dynamic Slab | jemalloc | malloc |
|------|-----------|-------------|----------|--------|
| Single size (32B) | **5.7** | 6.7 | 9.3 | 6.9 |
| Mixed sizes | **6.7** | 16.4 | 9.9 | 6.8 |
| Batch hold (500 objects) | 31.6 | 258.3 | **2.5** | 1.7 |

Slab is now **16% faster** in multi-threaded single-size workloads (6.8→5.7 ns/op) and beats malloc for the first time. The mixed-size result (6.7 ns/op) shows slight regression due to radix tree overhead for pointer→slab_id lookup — this trades O(n) traversal cost for memory safety. Dynamic Slab's single-size performance improved 29% (9.4→6.7) via better inlining. The batch-hold pattern still exposes TLC overflow behavior as expected.

### Calloc (zero-initialized)

| Size | Slab | jemalloc | calloc |
|------|------|----------|--------|
| 32B | **3.3** | 6.3 | 4.3 |
| 256B | **3.9** | 7.4 | 5.2 |
| 1024B | **5.7** | 9.1 | 7.2 |
| 4096B | **13.9** | 18.5 | 17.2 |

Slab's calloc improved 10-13% across all sizes and remains consistently faster than both glibc's calloc and jemalloc.

### Known limitations

- **`free` requires the size.** `slab::free(ptr, size)` requires the caller to pass the allocation size. This is the primary source of the performance advantage over jemalloc — but it means Slab cannot be a drop-in heap replacement. It fits best in contexts where objects have a known, fixed type/size (object pools, per-request buffers, typed containers).
- **Batch-hold pattern**: When threads hold more than ~128 live objects simultaneously, Slab's TLC overflows and falls back to mutex-protected pool operations, causing significant throughput degradation under high concurrency.
- **Radix tree overhead in mixed-size MT**: The radix tree for O(1) pointer→slab_id lookup adds ~4-5ns overhead in multi-threaded mixed-size workloads compared to the previous O(n) traversal. This trade-off enables memory safety and correct free() behavior across slabs.
- **malloc still fastest for immediate-free**: glibc's per-thread fastbins remain extremely optimized for the single-threaded alloc→immediate-free pattern, though the gap has narrowed significantly (Slab now only 0.7ns slower).

