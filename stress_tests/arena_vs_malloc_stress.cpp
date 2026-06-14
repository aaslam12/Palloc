#include <benchmark/benchmark.h>
#include "arena.h"
#include <cstdlib>
#include <unistd.h>
#include <vector>
using namespace AL;

static void BM_Arena_SmallAllocs(benchmark::State& state)
{
    const size_t page = static_cast<size_t>(getpagesize());
    const int n = 200000;
    for (auto _ : state)
    {
        state.PauseTiming(); arena<> a(page * 1000); state.ResumeTiming();
        for (int i = 0; i < n; ++i) benchmark::DoNotOptimize(a.alloc(8));
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Arena_SmallAllocs)->Unit(benchmark::kMillisecond);

static void BM_Malloc_SmallAllocs(benchmark::State& state)
{
    const int n = 200000;
    std::vector<void*> ptrs; ptrs.reserve(n);
    for (auto _ : state)
    {
        for (int i = 0; i < n; ++i) ptrs.push_back(std::malloc(8));
        for (void* p : ptrs) std::free(p); ptrs.clear();
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Malloc_SmallAllocs)->Unit(benchmark::kMillisecond);

static void BM_Arena_ResetCycle(benchmark::State& state)
{
    const size_t page = static_cast<size_t>(getpagesize());
    arena<> a(page * 32);
    for (auto _ : state)
    {
        for (int i = 0; i < 1000; ++i) benchmark::DoNotOptimize(a.alloc(100));
        a.reset();
    }
    state.SetItemsProcessed(state.iterations() * 1001);
}
BENCHMARK(BM_Arena_ResetCycle);

static void BM_Malloc_FreeCycle(benchmark::State& state)
{
    std::vector<void*> ptrs; ptrs.reserve(1000);
    for (auto _ : state)
    {
        for (int i = 0; i < 1000; ++i) ptrs.push_back(std::malloc(100));
        for (void* p : ptrs) std::free(p); ptrs.clear();
    }
    state.SetItemsProcessed(state.iterations() * 2000);
}
BENCHMARK(BM_Malloc_FreeCycle);

static void BM_Arena_MixedSizes(benchmark::State& state)
{
    const size_t page = static_cast<size_t>(getpagesize());
    for (auto _ : state)
    {
        state.PauseTiming(); arena<> a(page * 500); state.ResumeTiming();
        for (int i = 0; i < 50000; ++i) benchmark::DoNotOptimize(a.alloc(8 << (i % 4)));
    }
    state.SetItemsProcessed(state.iterations() * 50000);
}
BENCHMARK(BM_Arena_MixedSizes)->Unit(benchmark::kMillisecond);

static void BM_Malloc_MixedSizes(benchmark::State& state)
{
    std::vector<void*> ptrs; ptrs.reserve(50000);
    for (auto _ : state)
    {
        for (int i = 0; i < 50000; ++i) ptrs.push_back(std::malloc(8 << (i % 4)));
        for (void* p : ptrs) std::free(p); ptrs.clear();
    }
    state.SetItemsProcessed(state.iterations() * 50000);
}
BENCHMARK(BM_Malloc_MixedSizes)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
