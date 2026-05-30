# Profiling Artifacts

This directory contains lightweight generated profiling artifacts referenced from `docs/BENCHMARKS.md` and `docs/PROFILING.md`.

- `flamegraphs/`: SVG flamegraphs generated from `perf record`.
- `perf/`: text `perf stat` snapshots.

Large raw `perf.data`, `perf.script`, and folded-stack intermediates are intentionally not stored.
