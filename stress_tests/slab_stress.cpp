#include <benchmark/benchmark.h>
#include "slab.h"
#include <vector>
using namespace AL;

static void BM_Slab_MixedSizes(benchmark::State& state)
{
    const int allocs = 32;
    static const size_t sizes[] = {32, 64, 128, 256};
    default_slab s;
    std::vector<std::pair<void*, size_t>> ptrs;
    ptrs.reserve(allocs);
    for (auto _ : state)
    {
        for (int i = 0; i < allocs; ++i)
        {
            size_t sz = sizes[i % 4];
            ptrs.push_back({s.palloc(sz), sz});
        }
        for (auto& [ptr, sz] : ptrs)
            s.free(ptr, sz);
        ptrs.clear();
    }
    state.SetItemsProcessed(state.iterations() * allocs * 2);
}
BENCHMARK(BM_Slab_MixedSizes);

static void BM_Slab_RapidSingleSize(benchmark::State& state)
{
    default_slab s;
    for (auto _ : state)
    {
        void* ptr = s.palloc(64);
        benchmark::DoNotOptimize(ptr);
        s.free(ptr, 64);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_RapidSingleSize);
