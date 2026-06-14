// ═══════════════════════════════════════════════════════════════════════════════
// Fragmentation Stress Test — Realistic Allocator Benchmark
//
// Simulates a long-running server workload where objects of mixed sizes
// are allocated and freed with varying lifetimes.
//
// Allocators tested: Slab, jemalloc, malloc
// ═══════════════════════════════════════════════════════════════════════════════

#include <benchmark/benchmark.h>
#include "slab.h"

#include <jemalloc/jemalloc.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace AL;
using Clock = std::chrono::high_resolution_clock;

// ─── Test parameters ─────────────────────────────────────────────────────────

static constexpr size_t CHURN_OPS = 50'000;
static constexpr size_t NUM_SLOTS = 50'000;

static constexpr size_t SIZES[] = {16, 32, 64, 128, 256, 512};
static constexpr size_t NUM_SIZES = sizeof(SIZES) / sizeof(SIZES[0]);

// ─── Slot-based workload ──────────────────────────────────────────────────────

struct Slot
{
    void* ptr = nullptr;
    size_t size = 0;
};

struct BenchResult
{
    const char* name;
    size_t ops;
};

template <typename AllocFn, typename FreeFn>
BenchResult run_fragmentation(const char* name, AllocFn alloc_fn, FreeFn free_fn)
{
    std::vector<Slot> slots(NUM_SLOTS);
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> slot_dist(0, NUM_SLOTS - 1);
    std::uniform_int_distribution<size_t> size_idx_dist(0, NUM_SIZES - 1);

    size_t ops = 0;

    // Phase 1: Fill all slots
    for (size_t i = 0; i < NUM_SLOTS; i++)
    {
        size_t sz = SIZES[size_idx_dist(rng)];
        void* mem = alloc_fn(sz);
        if (mem)
        {
            std::memset(mem, static_cast<int>(i & 0xFF), sz);
            benchmark::DoNotOptimize(mem);
            slots[i] = {mem, sz};
        }
    }

    // Phase 2: Churn
    while (ops < CHURN_OPS)
    {
        size_t slot_idx = slot_dist(rng);
        Slot& slot = slots[slot_idx];

        if (slot.ptr)
        {
            volatile uint8_t v = *static_cast<uint8_t*>(slot.ptr);
            (void)v;
            free_fn(slot.ptr, slot.size);
            slot.ptr = nullptr;
            slot.size = 0;
        }

        size_t new_sz = SIZES[size_idx_dist(rng)];
        void* mem = alloc_fn(new_sz);
        if (mem)
        {
            std::memset(mem, static_cast<int>(ops & 0xFF), new_sz);
            benchmark::DoNotOptimize(mem);
            slot.ptr = mem;
            slot.size = new_sz;
        }

        ops++;
    }

    for (auto& slot : slots)
    {
        if (slot.ptr) { free_fn(slot.ptr, slot.size); slot.ptr = nullptr; }
    }

    return {name, ops};
}

// ─── Benchmarks ───────────────────────────────────────────────────────────────

static void BM_Fragmentation_Slab(benchmark::State& state)
{
    for (auto _ : state)
    {
        default_slab s{};
        auto r = run_fragmentation("Slab", [&](size_t sz)->void*{return s.palloc(sz);}, [&](void*p,size_t sz){s.free(p,sz);});
        benchmark::DoNotOptimize(r.ops);
    }
}
BENCHMARK(BM_Fragmentation_Slab)->Unit(benchmark::kMillisecond);

static void BM_Fragmentation_Jemalloc(benchmark::State& state)
{
    for (auto _ : state)
    {
        auto r = run_fragmentation("jemalloc", [](size_t sz)->void*{return mallocx(sz,0);}, [](void*p,size_t){dallocx(p,0);});
        benchmark::DoNotOptimize(r.ops);
    }
}
BENCHMARK(BM_Fragmentation_Jemalloc)->Unit(benchmark::kMillisecond);

static void BM_Fragmentation_Malloc(benchmark::State& state)
{
    for (auto _ : state)
    {
        auto r = run_fragmentation("malloc", [](size_t sz)->void*{return std::malloc(sz);}, [](void*p,size_t){std::free(p);});
        benchmark::DoNotOptimize(r.ops);
    }
}
BENCHMARK(BM_Fragmentation_Malloc)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
