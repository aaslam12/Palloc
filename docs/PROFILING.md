# Palloc Profiling Guide

This document describes how the perf artifacts in `docs/artifacts/` were produced and how to regenerate them. The benchmark tables in `BENCHMARKS.md` remain the source of truth for performance claims; flamegraphs and counters are explanatory evidence for hot paths and cache-coherency behavior.

## Environment

Current captured artifacts were generated on:

| Field | Value |
|-------|-------|
| OS | Linux `7.0.9-zen1-1-zen` |
| CPU | 11th Gen Intel Core i5-11500, 6 cores / 12 threads |
| Compiler | GCC 16.1.1 |
| perf | `perf version 7.0.9-1` |
| Build | Release, `-O3 -flto` |
| `perf_event_paranoid` | `2` |

The flamegraph tools used here are:

```bash
perf --version
stackcollapse-perf.pl --help
flamegraph.pl --help
```

## Build

Build the relevant Release targets directly.

```bash
python build.py --config Release --build-only
cmake --build build/Release --target slab_tlc_stress
cmake --build build/Release --target pool_thread_stress
cmake --build build/Release --target pool_stress
```

## Flamegraphs

Use DWARF call graphs because Release builds may omit frame pointers. If you want cleaner call stacks, rebuild with frame pointers and repeat the same commands.

```bash
mkdir -p docs/artifacts/perf docs/artifacts/flamegraphs

perf record -F 999 -g --call-graph dwarf \
  -o docs/artifacts/perf/slab_tlc_stress.perf.data \
  -- build/Release/slab_tlc_stress
perf script -i docs/artifacts/perf/slab_tlc_stress.perf.data \
  > docs/artifacts/perf/slab_tlc_stress.perf.script
stackcollapse-perf.pl docs/artifacts/perf/slab_tlc_stress.perf.script \
  > docs/artifacts/perf/slab_tlc_stress.folded
flamegraph.pl docs/artifacts/perf/slab_tlc_stress.folded \
  > docs/artifacts/flamegraphs/slab_tlc_stress.svg

perf record -F 999 -g --call-graph dwarf \
  -o docs/artifacts/perf/pool_thread_stress.perf.data \
  -- build/Release/pool_thread_stress
perf script -i docs/artifacts/perf/pool_thread_stress.perf.data \
  > docs/artifacts/perf/pool_thread_stress.perf.script
stackcollapse-perf.pl docs/artifacts/perf/pool_thread_stress.perf.script \
  > docs/artifacts/perf/pool_thread_stress.folded
flamegraph.pl docs/artifacts/perf/pool_thread_stress.folded \
  > docs/artifacts/flamegraphs/pool_thread_stress.svg
```

Raw `perf.data`, `perf.script`, and folded stack files are intentionally not kept in the repository because they can be large and are mechanically regenerated from the commands above.

## Counter Snapshots

These counters track general CPU cost, cache behavior, and Intel HITM-style modified-line sharing:

```bash
perf stat \
  -e cycles,instructions,cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses,mem_load_l3_hit_retired.xsnp_hitm,ocr.demand_rfo.l3_hit.snoop_hitm \
  -- build/Release/slab_tlc_stress \
  > docs/artifacts/perf/slab_tlc_stress.perfstat.txt 2>&1

perf stat \
  -e cycles,instructions,cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses,mem_load_l3_hit_retired.xsnp_hitm,ocr.demand_rfo.l3_hit.snoop_hitm \
  -- build/Release/pool_thread_stress \
  > docs/artifacts/perf/pool_thread_stress.perfstat.txt 2>&1

perf stat \
  -e cycles,instructions,cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses,mem_load_l3_hit_retired.xsnp_hitm,ocr.demand_rfo.l3_hit.snoop_hitm \
  -- build/Release/pool_stress \
  > docs/artifacts/perf/pool_stress.perfstat.txt 2>&1
```

Interpretation rules:

- `pool_stress` is the single-thread direct pool baseline; HITM should be near zero.
- `pool_thread_stress` intentionally hammers one shared pool, so HITM traffic is expected.
- `slab_tlc_stress` is useful for checking TLC and fallback behavior, but aggregate counters cannot attribute HITM to a specific cache line.

## Cache-Line Attribution

For exact false-sharing attribution, use `perf c2c`:

```bash
perf c2c record -o docs/artifacts/perf/slab_tlc_stress.c2c.data \
  --all-user -- build/Release/slab_tlc_stress
perf c2c report -i docs/artifacts/perf/slab_tlc_stress.c2c.data \
  --stdio --show-all > docs/artifacts/perf/slab_tlc_stress.c2c.txt
```

On the current machine this is blocked:

```text
perf_event_paranoid setting is 2
Failure to open event 'cpu/mem-loads,ldlat=30/P'
Failure to open event 'cpu/mem-stores/P'
```

Lowering `kernel.perf_event_paranoid` or running with the required perf capabilities is needed before committing a cache-line attribution report.

## Diagram Source

The architecture diagram in the README is Mermaid Markdown so it renders directly on GitHub and does not require generated image files.
