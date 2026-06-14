#include <benchmark/benchmark.h>
#include "pool.h"
#include <vector>
using namespace AL;

static void BM_Pool_AllocFree(benchmark::State& state)
{
    pool<> p(64, 1000000);
    for (auto _ : state)
    {
        void* ptr = p.alloc();
        benchmark::DoNotOptimize(ptr);
        p.free(ptr);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Pool_AllocFree);

static void BM_Pool_PartialCycle(benchmark::State& state)
{
    const int allocs = 50000;
    pool<> p(128, 1000000);
    std::vector<void*> ptrs;
    ptrs.reserve(allocs);
    for (auto _ : state)
    {
        for (int i = 0; i < allocs; ++i)
            ptrs.push_back(p.alloc());
        for (void* ptr : ptrs)
            p.free(ptr);
        ptrs.clear();
    }
    state.SetItemsProcessed(state.iterations() * allocs * 2);
}
BENCHMARK(BM_Pool_PartialCycle)->Unit(benchmark::kMillisecond);

static void BM_Pool_FullExhaustion(benchmark::State& state)
{
    const int block_count = 100000;
    pool<> p(128, block_count);
    std::vector<void*> ptrs;
    ptrs.reserve(block_count);
    for (auto _ : state)
    {
        for (int i = 0; i < block_count; ++i)
            ptrs.push_back(p.alloc());
        for (void* ptr : ptrs)
            p.free(ptr);
        ptrs.clear();
    }
    state.SetItemsProcessed(state.iterations() * block_count * 2);
}
BENCHMARK(BM_Pool_FullExhaustion)->Unit(benchmark::kMillisecond);
