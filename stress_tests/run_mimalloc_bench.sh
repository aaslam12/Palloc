#!/usr/bin/env bash
# run mimalloc-bench workloads under palloc (dynamic_slab shim), jemalloc, and system malloc
# output saved to docs/mimalloc_bench_results.txt

set -euo pipefail

BENCH_DIR=/home/al/Projects/mimalloc-bench/out/bench
BENCH_SRC=/home/al/Projects/mimalloc-bench/bench
PALLOC_SHIM=/home/al/Projects/Palloc/build/Release/libpalloc_shim.so
JEMALLOC_SO=/usr/lib/libjemalloc.so
OUTPUT=/home/al/Projects/Palloc/docs/mimalloc_bench_results.txt
RUNS=3
PROCS=$(nproc)

mkdir -p "$(dirname "$OUTPUT")"
: > "$OUTPUT"

log() { echo "$*" | tee -a "$OUTPUT"; }

run_bench() {
    local label="$1" preload="$2"; shift 2
    log ""
    log "--- $label ---"
    for i in $(seq 1 $RUNS); do
        log "  [run $i]"
        if [[ -n "$preload" ]]; then
            LD_PRELOAD="$preload" /usr/bin/time -f "%e sec %M KB" "$@" 2>&1 | tee -a "$OUTPUT" || true
        else
            /usr/bin/time -f "%e sec %M KB" "$@" 2>&1 | tee -a "$OUTPUT" || true
        fi
    done
}

bench() {
    local name="$1"; shift
    run_bench "$name [malloc]"  ""              "$@"
    run_bench "$name [palloc]"  "$PALLOC_SHIM"  "$@"
    if [[ -f "$JEMALLOC_SO" ]]; then
        run_bench "$name [jemalloc]" "$JEMALLOC_SO" "$@"
    fi
}

log "=== mimalloc-bench results ==="
log "date: $(date)"
log "host: $(uname -srm)"
log "cpus: $PROCS"
log "palloc shim: $PALLOC_SHIM (dynamic_slab)"
log "jemalloc: $JEMALLOC_SO"
log ""

cd "$BENCH_DIR"

bench "cfrac"          ./cfrac 17545186520507317056371138836327483792789528
bench "espresso"       ./espresso "$BENCH_SRC/espresso/largest.espresso"
bench "barnes"         sh -c "./barnes < $BENCH_SRC/barnes/input"
bench "larson"         ./larson 5 8 1000 5000 100 4141 "$PROCS"
bench "larson-sized"   ./larson-sized 5 8 1000 5000 100 4141 "$PROCS"
bench "alloc-test1"    ./alloc-test 1
bench "alloc-testN"    ./alloc-test "$PROCS"
bench "cache-scratch1" ./cache-scratch 1 1000 1 2000000 "$PROCS"
bench "cache-scratchN" ./cache-scratch "$PROCS" 1000 1 2000000 "$PROCS"
bench "cache-thrash1"  ./cache-thrash 1 1000 1 2000000 "$PROCS"
bench "cache-thrashN"  ./cache-thrash "$PROCS" 1000 1 2000000 "$PROCS"
bench "xmalloc-test"   ./xmalloc-test -w "$PROCS" -t 5 -s 64
bench "malloc-large"   ./malloc-large
bench "mstress"        ./mstress "$PROCS" 50 25
bench "mleak5"         ./mleak 5
bench "mleak50"        ./mleak 50
bench "rptest"         ./rptest "$PROCS" 0 1 2 500 1000 100 8 16000
bench "glibc-simple"   ./glibc-simple
bench "glibc-thread"   ./glibc-thread "$PROCS"

log ""
log "=== done ==="
echo "results saved to $OUTPUT"
