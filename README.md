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

## Benchmarks

Benchmarked on Linux (12-core Intel i5 11th gen), compiled with GCC `-O3`. All numbers are ns/op (lower is better).

### Single-threaded alloc+free by size

| Size | Slab (TLC) | Dynamic Slab | jemalloc | malloc |
|------|-----------|-------------|----------|--------|
| 8B | **2.6** | 4.7 | 5.8 | 2.3 |
| 16B | **2.6** | 5.0 | 5.7 | 2.4 |
| 32B | **2.6** | 5.8 | 5.8 | 2.3 |
| 64B | **2.6** | 6.1 | 5.8 | 2.3 |
| 128B | **2.6** | 6.5 | 5.7 | 2.5 |
| 256B | **2.6** | 7.1 | 5.8 | 2.5 |
| 512B | **2.6** | 7.4 | 6.0 | 2.7 |
| 1024B | **2.6** | 8.0 | 6.2 | 3.2 |
| 2048B | **2.6** | 8.4 | 6.9 | 4.2 |
| 4096B | **2.6** | 9.0 | 7.7 | 5.9 |

Slab's TLC gives it a ~2x advantage over jemalloc here. Two structural reasons explain the gap — both tied to the benchmark conditions:

1. **Caller-supplied size.** `slab::free(ptr, size)` requires the caller to pass the size. This lets `size_to_index()` resolve the pool in a single `bit_width` instruction, with no pointer provenance lookup. jemalloc's `free(ptr)` must walk a radix tree keyed on address ranges to find the owning arena and size class — that's 2–3 cache misses on a cold path. The 2x gap is largely this lookup cost.

2. **Simpler TLC.** Slab's thread-local cache has no GC watermarks, no stats counters, and no background-thread coordination. Every alloc/free in the hot path is an array index increment/decrement on an already-hot cache line.

> **These conditions don't always hold in practice.** The 2x figure applies when: (a) the caller tracks sizes, (b) objects are short-lived so TLC entries stay L1-hot between alloc and free, and (c) threads don't hold more than ~128 live objects simultaneously. Multi-threaded workloads that hold many live objects degrade significantly (see batch-hold row below). For general-purpose heap replacement, jemalloc is a better fit.

### Linear allocation (alloc only, no free)

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| malloc | 6.3 | 157.8 |
| Pool | 8.3 | 120.1 |
| Arena | 9.5 | 105.8 |
| jemalloc | 11.7 | 85.5 |

Arena and Pool are competitive with malloc for pure allocation throughput. jemalloc's metadata overhead makes it slowest here.

### Fixed-size alloc+free (single-thread)

Single-threaded alloc+free pairs at 64B, 1M cycles.

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| malloc | 2.3 | 427.7 |
| Slab (TLC) | **2.5** | 402.6 |
| jemalloc | 5.8 | 171.4 |
| Pool | 7.8 | 128.8 |

### Batch alloc-then-free

256 objects allocated then freed together, 200K cycles, 64B.

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| Slab (TLC) | **5.8** | 171.4 |
| malloc | 6.4 | 156.7 |
| Dynamic Slab | 9.4 | 106.6 |
| jemalloc | 9.7 | 102.8 |

Slab's TLC handles delayed-free patterns faster than both jemalloc and Dynamic Slab. Dynamic Slab's O(n) free-list traversal is masked somewhat by the batch size.

### Multi-threaded (8 threads)

| Test | Slab (TLC) | Dynamic Slab | jemalloc | malloc |
|------|-----------|-------------|----------|--------|
| Single size (32B) | **6.8** | 9.4 | 9.5 | 7.2 |
| Mixed sizes | **6.4** | 10.0 | 9.2 | 7.2 |
| Batch hold (500 objects) | 24.0 | 344.7 | **2.6** | 1.7 |

Slab edges ahead of malloc on contention-heavy single-size and mixed-size MT tests via TLC avoiding lock contention. The batch-hold pattern exposes Slab's weakness: TLC entries flush when holding many objects, falling back to mutex-protected pool ops. Dynamic Slab's O(n) slab-node traversal to find the owning slab on `free()` makes it ~133x slower than jemalloc on this pattern.

### Calloc (zero-initialized)

| Size | Slab | jemalloc | calloc |
|------|------|----------|--------|
| 32B | **3.8** | 7.0 | 5.4 |
| 256B | **4.5** | 7.9 | 5.5 |
| 1024B | **6.5** | 10.6 | 8.8 |
| 4096B | **14.4** | 19.1 | 18.0 |

Slab's calloc is competitive with glibc's calloc and consistently faster than jemalloc.

### Known limitations

- **`free` requires the size.** `slab::free(ptr, size)` requires the caller to pass the allocation size. This is the primary source of the performance advantage over jemalloc — but it means Slab cannot be a drop-in heap replacement. It fits best in contexts where objects have a known, fixed type/size (object pools, per-request buffers, typed containers).
- **Batch-hold pattern**: When threads hold more than ~128 live objects simultaneously, Slab's TLC overflows and falls back to mutex-protected pool operations, causing significant throughput degradation under high concurrency.
- **Dynamic Slab free()**: O(n) slab-node traversal to find the owning slab on every `free()`. jemalloc uses a radix tree for O(1) pointer-to-arena lookup.
- **malloc advantage at small sizes**: glibc's per-thread fastbins are extremely optimized for the alloc→immediate-free pattern in single-threaded code.

