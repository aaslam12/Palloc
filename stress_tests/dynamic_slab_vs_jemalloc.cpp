#include <benchmark/benchmark.h>
#include "slab.h"
#include <jemalloc/jemalloc.h>
#include <vector>
using namespace AL;

static constexpr size_t DSVJSIZES[] = {8,16,32,64,128,256,512,1024};

static void BM_Slab_LongLived(benchmark::State& state)
{
    constexpr size_t hold = 1000, sz = 64;
    default_slab s;
    std::vector<void*> ptrs(hold);
    for (auto _ : state)
    {
        for (size_t i = 0; i < hold; ++i) ptrs[i] = s.palloc(sz);
        for (size_t i = 0; i < hold; ++i) s.free(ptrs[i], sz);
    }
    state.SetItemsProcessed(state.iterations() * hold * 2);
}
BENCHMARK(BM_Slab_LongLived);

static void BM_Jemalloc_LongLived(benchmark::State& state)
{
    constexpr size_t hold = 1000, sz = 64;
    std::vector<void*> ptrs(hold);
    for (auto _ : state)
    {
        for (size_t i = 0; i < hold; ++i) ptrs[i] = mallocx(sz, 0);
        for (size_t i = 0; i < hold; ++i) dallocx(ptrs[i], 0);
    }
    state.SetItemsProcessed(state.iterations() * hold * 2);
}
BENCHMARK(BM_Jemalloc_LongLived);

static void BM_Slab_MT_LongLived(benchmark::State& state)
{
    static default_slab* s = nullptr;
    if (state.thread_index() == 0) s = new default_slab();
    constexpr size_t hold = 500, sz = 32;
    std::vector<void*> ptrs(hold);
    for (auto _ : state)
    {
        for (size_t i = 0; i < hold; ++i) ptrs[i] = s->palloc(sz);
        for (size_t i = 0; i < hold; ++i) s->free(ptrs[i], sz);
    }
    if (state.thread_index() == 0) { delete s; s = nullptr; }
    state.SetItemsProcessed(state.iterations() * hold * 2);
}
BENCHMARK(BM_Slab_MT_LongLived)->ThreadRange(1, 8);

static void BM_Jemalloc_MT_LongLived(benchmark::State& state)
{
    constexpr size_t hold = 500, sz = 32;
    std::vector<void*> ptrs(hold);
    for (auto _ : state)
    {
        for (size_t i = 0; i < hold; ++i) ptrs[i] = mallocx(sz, 0);
        for (size_t i = 0; i < hold; ++i) dallocx(ptrs[i], 0);
    }
    state.SetItemsProcessed(state.iterations() * hold * 2);
}
BENCHMARK(BM_Jemalloc_MT_LongLived)->ThreadRange(1, 8);

static void BM_Slab_Mixed(benchmark::State& state)
{
    static default_slab* s = nullptr;
    if (state.thread_index() == 0) s = new default_slab();
    size_t i = static_cast<size_t>(state.thread_index());
    for (auto _ : state) { size_t sz=DSVJSIZES[i%8]; void* p=s->palloc(sz); benchmark::DoNotOptimize(p); if(p) s->free(p,sz); ++i; }
    if (state.thread_index() == 0) { delete s; s = nullptr; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_Mixed)->ThreadRange(1, 8);

static void BM_Jemalloc_Mixed(benchmark::State& state)
{
    size_t i = 0;
    for (auto _ : state) { size_t sz=DSVJSIZES[i%8]; void* p=mallocx(sz,0); benchmark::DoNotOptimize(p); dallocx(p,0); ++i; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Jemalloc_Mixed)->ThreadRange(1, 8);

BENCHMARK_MAIN();
