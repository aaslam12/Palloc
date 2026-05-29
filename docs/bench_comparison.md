# Benchmark Comparison: refactor-slab vs cbf35fd

Baseline: commit `cbf35fd` (TLC stress refactor)
Current:  branch `refactor-slab` (Changes 1-4: atomic bitmap, on-demand growth, shrink, two-level bitmap)

Each test run 3x; values are averages. Positive % = current is slower, negative % = current is faster.

## slab_stress

| Test | Baseline | Current | Delta |
|------|----------|---------|-------|
| Mixed sizes (64M ops) ops/s | 250039333 | 234257000 | -6.3% |
| Rapid single-size (20M ops) ops/s | 327600000 | 307050667 | -6.3% |

## slab_tlc_stress

| Test | Baseline | Current | Delta |
|------|----------|---------|-------|
| 8B TLC ns/op | 0.99 | 2.03 | +104.5% |
| 64B TLC ns/op | 1.98 | 2.08 | +4.8% |
| TLC batch pressure ns/op | 2.56 | 3.08 | +20.2% |
| Concurrent throughput ops/s | 2424845596.67 | 2100402945.67 | -13.4% |
| Multi-slab no-churn ns/op | 12.05 | 7.46 | -38.1% |
| Multi-slab churn ns/op | 49.20 | 34.92 | -29.0% |

## allocator_showdown

### Single-threaded alloc+free (cycles/op)

| Size | Baseline Slab | Current Slab | Delta | Baseline DynSlab | Current DynSlab |
|------|--------------|--------------|-------|-----------------|-----------------|
| 8B | 7.5 | 8.0 | +5.8% | 16.0 | 17.4 |
| 16B | 7.8 | 7.9 | +2.1% | 16.9 | 17.9 |
| 32B | 7.9 | 8.1 | +3.0% | 17.0 | 18.1 |
| 64B | 8.0 | 8.2 | +2.1% | 17.0 | 18.2 |
| 128B | 7.8 | 8.1 | +3.8% | 17.0 | 18.2 |
| 256B | 7.9 | 8.0 | +1.3% | 17.1 | 18.3 |
| 512B | 8.0 | 8.2 | +2.1% | 16.9 | 18.2 |
| 1024B | 7.9 | 8.0 | +2.1% | 16.8 | 18.3 |
| 2048B | 7.7 | 8.0 | +3.9% | 16.8 | 19.2 |
| 4096B | 7.8 | 8.2 | +5.6% | 16.9 | 18.2 |

### Other tests (cycles/op)

| Test | Allocator | Baseline | Current | Delta |
|------|-----------|----------|---------|-------|
| Linear alloc (no free) | Arena | 13.0 | 13.1 | +0.5% |
| Linear alloc (no free) | malloc | 14.5 | 13.8 | -4.4% |
| Linear alloc (no free) | jemalloc | 30.3 | 29.9 | -1.1% |
| Fixed-size alloc+free | Slab (TLC) | 4.3 | 4.2 | -2.3% |
| Fixed-size alloc+free | malloc | 5.9 | 6.5 | +10.2% |
| Fixed-size alloc+free | jemalloc | 12.1 | 12.3 | +1.4% |
| Batch alloc-then-free | Slab (TLC) | 9.8 | 12.0 | +22.5% |
| Batch alloc-then-free | malloc | 15.7 | 15.7 | +0.2% |
| MT single-size 32B | Slab (TLC) | 2.7 | 0.9 | -65.9% |
| MT single-size 32B | malloc | 1.3 | 1.2 | -7.9% |
| MT mixed sizes | Slab (TLC) | 3.2 | 2.3 | -27.4% |
| MT mixed sizes | malloc | 1.6 | 1.5 | -6.3% |
| MT batch hold 500 | Slab (TLC) | ~2.1 (invalid*) | 9.6 | — |
| MT batch hold 500 | malloc | 3.7 | 3.6 | -1.8% |

> \* Baseline MT batch hold 500 Slab number is invalid: the fixed-capacity baseline pool (256 blocks × 12 threads = 3072 slots needed for 500-hold × 12 threads = 6000) exhausted silently, most allocs returned nullptr. Baseline was measuring null-check cost, not real alloc/free. Current measures actual work.

## arena_vs_malloc_stress

| Test | Allocator | Baseline ops/s | Current ops/s | Delta |
|------|-----------|----------------|---------------|-------|
| Sequential small allocs | Arena | 110364000 | 113043667 | +2.4% |
| Sequential small allocs | malloc | 110364000 | 113043667 | +2.4% |
| Alloc/reset cycles | Arena | 220241333 | 220629333 | +0.2% |
| Alloc/reset cycles | malloc | 220241333 | 220629333 | +0.2% |
| Mixed sizes | Arena | 151657333 | 170623333 | +12.5% |
| Mixed sizes | malloc | 151657333 | 170623333 | +12.5% |

## pool_vs_malloc_stress

| Test | Allocator | Baseline ns/op | Current ns/op | Delta |
|------|-----------|----------------|---------------|-------|
| Fixed-size alloc+free (100M ops) | Pool | 14.82 | 15.43 | +4.1% |
| Fixed-size alloc+free (100M ops) | malloc | 16.92 | 16.80 | -0.7% |
| Full exhaustion+reuse (1M ops) | Pool | 13.33 | 14.17 | +6.3% |
| Full exhaustion+reuse (1M ops) | malloc | 29.60 | 30.41 | +2.7% |

## slab_vs_malloc_stress

| Test | Allocator | Baseline ns/op | Current ns/op | Delta |
|------|-----------|----------------|---------------|-------|
| Mixed sizes (2M ops) | Slab | 3.71 | 4.56 | +22.9% |
| Mixed sizes (2M ops) | malloc | 5.37 | 5.28 | -1.7% |
| Rapid single-size (2M ops) | Slab | 2.91 | 3.60 | +23.9% |
| Batch alloc delayed free (2M) | Slab | 3.95 | 4.99 | +26.4% |
| Batch alloc delayed free (2M) | malloc | 5.65 | 5.55 | -1.7% |

## order_book_sim (realistic workload)

### Single-threaded ns/op

| Allocator | Baseline | Current | Delta |
|-----------|----------|---------|-------|
| Pool | 53.3 | 56.8 | +6.5% |
| Slab (TLC) | 48.4 | 50.2 | +3.9% |
| Dynamic Slab | 61.1 | 51.7 | -15.4% |
| jemalloc | 52.8 | 54.0 | +2.3% |
| malloc | 52.8 | 54.0 | +2.3% |

### Multi-threaded ns/op

| Allocator | Baseline | Current | Delta |
|-----------|----------|---------|-------|
| Pool | 112.0 | 72.6 | -35.2% |
| Slab (TLC) | 8.5 | 8.8 | +3.5% |
| Dynamic Slab | 158.5 | 9.5 | -94.0% |
| jemalloc | 9.9 | 10.2 | +2.7% |
| malloc | 9.9 | 10.2 | +2.7% |

## market_data_replay

| Allocator | Baseline ns/msg | Current ns/msg | Delta |
|-----------|-----------------|----------------|-------|
| Arena (batch) | 16.1 | 16.0 | -0.4% |
| Slab (TLC) | 19.0 | 20.8 | +9.6% |
| Dynamic Slab | 22.3 | 22.7 | +1.9% |
| jemalloc | 26.8 | 26.7 | -0.4% |
| malloc | 26.8 | 26.7 | -0.4% |

## fragmentation_stress

| Allocator | Baseline ns/op | Current ns/op | Delta |
|-----------|----------------|---------------|-------|
| Slab (TLC) | N/A | 30.2 |  |
| Dynamic Slab | 615.2 | 33.2 | -94.6% |
| jemalloc | 43.2 | 45.4 | +5.1% |
| malloc | 43.2 | 45.4 | +5.1% |

## producer_consumer_sim

| Allocator | Baseline ns/msg | Current ns/msg | Delta |
|-----------|-----------------|----------------|-------|
| Slab (TLC) | 29.6 | 27.1 | -8.5% |
| Dynamic Slab | 41.9 | 30.9 | -26.2% |
| malloc | 25.5 | 27.0 | +5.8% |
| jemalloc | 25.5 | 27.0 | +5.8% |
| Pool | 143.4 | 134.4 | -6.3% |

## slab_vs_jemalloc (new — current branch only)

| Test | Slab (TLC) ns/op | jemalloc ns/op | Slab speedup |
|------|-----------------|----------------|-------------|
| ST long-lived hold (1K hold, 1K cycles) | 5.6 | 9.1 | 1.62x |
| MT long-lived (8 threads, 500 hold) | 5.2 | 2.2 | 0.43x |
| MT mixed sizes (8 threads) | 1.8 | 1.9 | 1.03x |

---

*All values are averages of 3 runs. Timing: wall-clock ns/op or RDTSC cycles/op as reported by each test.*