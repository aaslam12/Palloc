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
- [Benchmarks](#benchmarks)
  - [Single-threaded by size](#single-threaded-allocfree-by-size)
  - [Linear allocation](#linear-allocation-alloc-only-no-free)
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
- 0 data races, 0 memory errors across 81 tests + 40 thread-safety tests

---

## Getting Started

### Requirements

- Unix Operating System (library uses `mmap` `munmap`)
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

---

## Benchmarks

Benchmarked on Linux (12-core Intel i5 11th gen), compiled with GCC `-O3`. All numbers are ns/op (lower is better).

### Single-threaded alloc+free by size

| Size | Slab (TLC) | Dynamic Slab | jemalloc | malloc |
|------|-----------|-------------|----------|--------|
| 8B | **3.2** | 4.7 | 5.7 | 2.3 |
| 16B | **3.3** | 5.4 | 5.6 | 2.3 |
| 32B | **3.3** | 6.0 | 5.7 | 2.3 |
| 64B | **3.3** | 6.7 | 5.7 | 2.3 |
| 128B | **3.4** | 7.2 | 5.7 | 2.4 |
| 256B | **3.7** | 7.7 | 5.8 | 2.5 |
| 512B | **3.7** | 8.3 | 6.0 | 2.7 |
| 1024B | **4.0** | 9.0 | 6.2 | 3.2 |
| 2048B | **3.7** | 9.1 | 6.8 | 4.1 |
| 4096B | **3.7** | 9.7 | 7.7 | 5.7 |

Slab's TLC gives it a consistent ~1.7x advantage over jemalloc across all sizes. malloc is fastest at small sizes since glibc's fastbins are highly optimized for ST alloc+immediate-free, but Slab closes the gap at larger sizes.

### Linear allocation (alloc only, no free)

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| malloc | 6.3 | 158 |
| Pool | 8.2 | 121 |
| Arena | 8.9 | 113 |
| jemalloc | 11.7 | 86 |

Arena and Pool are competitive with malloc for pure allocation throughput. jemalloc's metadata overhead makes it slowest here.

### Multi-threaded (8 threads)

| Test | Slab (TLC) | Dynamic Slab | jemalloc | malloc |
|------|-----------|-------------|----------|--------|
| Single size (32B) | **6.9** | 9.4 | 9.2 | 7.3 |
| Mixed sizes | **6.5** | 9.5 | 9.0 | 6.5 |
| Batch hold (500 objects) | 25.0 | 228 | **2.5** | 1.6 |

Slab wins the contention-heavy single-size and mixed-size MT tests. The batch-hold pattern exposes Slab's weakness: TLC entries get flushed when holding many objects, falling back to the mutex-protected pool. Dynamic Slab's O(n) free-list walk makes it ~90x slower than jemalloc on this pattern.

### Calloc (zero-initialized)

| Size | Slab | jemalloc | calloc |
|------|------|----------|--------|
| 32B | 4.5 | 6.3 | 4.4 |
| 256B | 5.0 | 6.8 | 5.5 |
| 1024B | 6.5 | 9.2 | 7.4 |
| 4096B | 13.7 | 18.4 | 17.5 |

Slab's calloc is competitive with glibc's calloc and consistently faster than jemalloc.

### Known limitations

- **Batch-hold pattern**: When threads hold many allocations before freeing, Slab's TLC overflows and falls back to mutex-protected pool operations
- **Dynamic Slab free()**: O(n) slab-node traversal to find the owning slab. jemalloc uses a radix tree for O(1) pointer-to-arena lookup
- **malloc advantage at small sizes**: glibc's per-thread fastbins are extremely optimized for the alloc→immediate-free pattern in single-threaded code

