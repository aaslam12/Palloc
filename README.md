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
- 0 data races, 0 memory errors across 81 tests + 40 thread-safety tests

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
| 8B | **2.8** | 4.4 | 5.7 | 2.3 |
| 16B | **2.9** | 5.5 | 6.0 | 2.4 |
| 32B | **3.2** | 6.3 | 6.1 | 2.4 |
| 64B | **3.4** | 6.7 | 6.2 | 2.4 |
| 128B | **3.6** | 7.4 | 6.2 | 2.6 |
| 256B | **3.6** | 8.1 | 6.3 | 2.6 |
| 512B | **3.6** | 8.5 | 6.3 | 2.9 |
| 1024B | **3.7** | 9.1 | 6.5 | 3.3 |
| 2048B | **3.6** | 9.6 | 7.0 | 4.3 |
| 4096B | **3.5** | 10.0 | 8.0 | 5.8 |

Slab's TLC gives it a consistent ~1.7–2x advantage over jemalloc across all sizes. malloc is fastest at small sizes since glibc's fastbins are highly optimized for ST alloc+immediate-free, but Slab closes the gap at larger sizes.

### Linear allocation (alloc only, no free)

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| malloc | 6.3 | 157.9 |
| Pool | 8.3 | 120.5 |
| Arena | 8.9 | 112.1 |
| jemalloc | 11.7 | 85.7 |

Arena and Pool are competitive with malloc for pure allocation throughput. jemalloc's metadata overhead makes it slowest here.

### Fixed-size alloc+free (single-thread)

Single-threaded alloc+free pairs at 64B, 1M cycles.

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| malloc | 2.4 | 416.2 |
| Slab (TLC) | **3.3** | 303.5 |
| jemalloc | 5.7 | 174.1 |
| Pool | 7.7 | 129.2 |

### Batch alloc-then-free

256 objects allocated then freed together, 200K cycles, 64B.

| Allocator | ns/op | MOps/s |
|-----------|-------|--------|
| malloc | 6.0 | 167.7 |
| Slab (TLC) | **6.7** | 150.1 |
| jemalloc | 9.3 | 107.5 |
| Dynamic Slab | 10.1 | 99.1 |

Slab's TLC handles delayed-free patterns competitively with malloc. Dynamic Slab performs comparably here as its O(n) free-list traversal is masked by the batch size.

### Multi-threaded (8 threads)

| Test | Slab (TLC) | Dynamic Slab | jemalloc | malloc |
|------|-----------|-------------|----------|--------|
| Single size (32B) | 6.6 | 8.8 | 8.5 | **6.5** |
| Mixed sizes | 7.3 | 10.6 | 9.6 | **7.1** |
| Batch hold (500 objects) | 24.1 | 211.1 | **2.5** | 1.6 |

malloc and Slab are neck-and-neck on the contention-heavy single-size and mixed-size MT tests; Slab's TLC avoids lock contention while malloc's per-thread fastbins achieve similar throughput. The batch-hold pattern exposes Slab's weakness: TLC entries get flushed when holding many objects, falling back to the mutex-protected pool. Dynamic Slab's O(n) free-list walk makes it ~84x slower than jemalloc on this pattern.

### Calloc (zero-initialized)

| Size | Slab | jemalloc | calloc |
|------|------|----------|--------|
| 32B | 4.2 | 6.3 | 4.1 |
| 256B | 5.0 | 7.3 | 5.2 |
| 1024B | 6.5 | 9.4 | 7.6 |
| 4096B | 13.8 | 18.5 | 17.3 |

Slab's calloc is competitive with glibc's calloc and consistently faster than jemalloc.

### Known limitations

- **Batch-hold pattern**: When threads hold many allocations before freeing, Slab's TLC overflows and falls back to mutex-protected pool operations
- **Dynamic Slab free()**: O(n) slab-node traversal to find the owning slab. jemalloc uses a radix tree for O(1) pointer-to-arena lookup
- **malloc advantage at small sizes**: glibc's per-thread fastbins are extremely optimized for the alloc→immediate-free pattern in single-threaded code

