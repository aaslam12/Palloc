#include <benchmark/benchmark.h>
#include "pool.h"
#include <cstdlib>
#include <vector>
using namespace AL;

static void BM_Pool_AllocFree_64(benchmark::State& state)
{
    pool<> p(64, 1000000);
    for (auto _ : state) { void* ptr = p.alloc(); benchmark::DoNotOptimize(ptr); p.free(ptr); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Pool_AllocFree_64);

static void BM_Malloc_AllocFree_64(benchmark::State& state)
{
    for (auto _ : state) { void* ptr = std::malloc(64); benchmark::DoNotOptimize(ptr); std::free(ptr); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Malloc_AllocFree_64);

static void BM_Pool_AllocFree_128(benchmark::State& state)
{
    pool<> p(128, 1000000);
    for (auto _ : state) { void* ptr = p.alloc(); benchmark::DoNotOptimize(ptr); p.free(ptr); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Pool_AllocFree_128);

static void BM_Malloc_AllocFree_128(benchmark::State& state)
{
    for (auto _ : state) { void* ptr = std::malloc(128); benchmark::DoNotOptimize(ptr); std::free(ptr); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Malloc_AllocFree_128);

static void BM_Pool_BatchCycle(benchmark::State& state)
{
    const int n = 50000;
    pool<> p(64, 1000000);
    std::vector<void*> ptrs;
    ptrs.reserve(n);
    for (auto _ : state)
    {
        for (int i = 0; i < n; ++i) ptrs.push_back(p.alloc());
        for (void* ptr : ptrs) p.free(ptr);
        ptrs.clear();
    }
    state.SetItemsProcessed(state.iterations() * n * 2);
}
BENCHMARK(BM_Pool_BatchCycle)->Unit(benchmark::kMillisecond);

static void BM_Malloc_BatchCycle(benchmark::State& state)
{
    const int n = 50000;
    std::vector<void*> ptrs;
    ptrs.reserve(n);
    for (auto _ : state)
    {
        for (int i = 0; i < n; ++i) ptrs.push_back(std::malloc(64));
        for (void* ptr : ptrs) std::free(ptr);
        ptrs.clear();
    }
    state.SetItemsProcessed(state.iterations() * n * 2);
}
BENCHMARK(BM_Malloc_BatchCycle)->Unit(benchmark::kMillisecond);

static void BM_Pool_Exhaust_256(benchmark::State& state)
{
    const int blocks = 5000;
    pool<> p(256, blocks);
    std::vector<void*> ptrs;
    ptrs.reserve(blocks);
    for (auto _ : state)
    {
        for (int i = 0; i < blocks; ++i) ptrs.push_back(p.alloc());
        for (void* ptr : ptrs) p.free(ptr);
        ptrs.clear();
    }
    state.SetItemsProcessed(state.iterations() * blocks * 2);
}
BENCHMARK(BM_Pool_Exhaust_256)->Unit(benchmark::kMillisecond);

static void BM_Malloc_Exhaust_256(benchmark::State& state)
{
    const int blocks = 5000;
    std::vector<void*> ptrs;
    ptrs.reserve(blocks);
    for (auto _ : state)
    {
        for (int i = 0; i < blocks; ++i) ptrs.push_back(std::malloc(256));
        for (void* ptr : ptrs) std::free(ptr);
        ptrs.clear();
    }
    state.SetItemsProcessed(state.iterations() * blocks * 2);
}
BENCHMARK(BM_Malloc_Exhaust_256)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
