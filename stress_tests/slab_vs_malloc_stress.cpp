#include <benchmark/benchmark.h>
#include "slab.h"
#include <cstdlib>
#include <vector>
using namespace AL;

static const size_t SIZES4[] = {32, 64, 128, 256};
static const size_t SIZES4B[] = {16, 32, 64, 128};

static void BM_Slab_MixedSizes(benchmark::State& state)
{
    default_slab s;
    std::vector<std::pair<void*,size_t>> ptrs; ptrs.reserve(100);
    for (auto _ : state)
    {
        for (int i = 0; i < 100; ++i) ptrs.push_back({s.palloc(SIZES4[i%4]), SIZES4[i%4]});
        for (auto& [p,sz] : ptrs) s.free(p, sz); ptrs.clear();
    }
    state.SetItemsProcessed(state.iterations() * 200);
}
BENCHMARK(BM_Slab_MixedSizes);

static void BM_Malloc_MixedSizes(benchmark::State& state)
{
    std::vector<void*> ptrs; ptrs.reserve(100);
    for (auto _ : state)
    {
        for (int i = 0; i < 100; ++i) ptrs.push_back(std::malloc(SIZES4[i%4]));
        for (void* p : ptrs) std::free(p); ptrs.clear();
    }
    state.SetItemsProcessed(state.iterations() * 200);
}
BENCHMARK(BM_Malloc_MixedSizes);

static void BM_Slab_Rapid64(benchmark::State& state)
{
    default_slab s;
    for (auto _ : state) { void* p = s.palloc(64); benchmark::DoNotOptimize(p); s.free(p, 64); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_Rapid64);

static void BM_Malloc_Rapid64(benchmark::State& state)
{
    for (auto _ : state) { void* p = std::malloc(64); benchmark::DoNotOptimize(p); std::free(p); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Malloc_Rapid64);

static void BM_Slab_Small(benchmark::State& state)
{
    default_slab s;
    size_t i = 0;
    for (auto _ : state) { size_t sz = 8+(i%4)*8; void* p = s.palloc(sz); benchmark::DoNotOptimize(p); s.free(p,sz); ++i; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_Small);

static void BM_Malloc_Small(benchmark::State& state)
{
    size_t i = 0;
    for (auto _ : state) { size_t sz = 8+(i%4)*8; void* p = std::malloc(sz); benchmark::DoNotOptimize(p); std::free(p); ++i; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Malloc_Small);

static void BM_Slab_BatchDelayed(benchmark::State& state)
{
    default_slab s;
    std::vector<std::pair<void*,size_t>> ptrs; ptrs.reserve(100);
    for (auto _ : state)
    {
        for (int i = 0; i < 100; ++i) ptrs.push_back({s.palloc(SIZES4B[i%4]), SIZES4B[i%4]});
        for (auto& [p,sz] : ptrs) s.free(p,sz); ptrs.clear();
    }
    state.SetItemsProcessed(state.iterations() * 200);
}
BENCHMARK(BM_Slab_BatchDelayed);

static void BM_Malloc_BatchDelayed(benchmark::State& state)
{
    std::vector<void*> ptrs; ptrs.reserve(100);
    for (auto _ : state)
    {
        for (int i = 0; i < 100; ++i) ptrs.push_back(std::malloc(SIZES4B[i%4]));
        for (void* p : ptrs) std::free(p); ptrs.clear();
    }
    state.SetItemsProcessed(state.iterations() * 200);
}
BENCHMARK(BM_Malloc_BatchDelayed);

BENCHMARK_MAIN();
