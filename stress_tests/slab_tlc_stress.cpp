#include <benchmark/benchmark.h>
#include "slab.h"
#include <array>
#include <vector>
using namespace AL;

static void BM_Slab_TLC(benchmark::State& state)
{
    const size_t sz = static_cast<size_t>(state.range(0));
    static default_slab* s = nullptr;
    if (state.thread_index() == 0) s = new default_slab();
    for (auto _ : state)
    {
        void* p = s->palloc(sz);
        benchmark::DoNotOptimize(p);
        s->free(p, sz);
    }
    if (state.thread_index() == 0) { delete s; s = nullptr; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_TLC)->Arg(8)->Arg(16)->Arg(32)->Arg(64)->Arg(128)->Arg(256)->Arg(512);

static void BM_Slab_TLC_BatchPressure(benchmark::State& state)
{
    constexpr size_t hold = 129;
    default_slab s;
    void* held[hold];
    for (auto _ : state)
    {
        for (size_t i = 0; i < hold; ++i) held[i] = s.palloc(32);
        for (size_t i = 0; i < hold; ++i) s.free(held[i], 32);
    }
    state.SetItemsProcessed(state.iterations() * hold * 2);
}
BENCHMARK(BM_Slab_TLC_BatchPressure);

static void BM_Slab_TLC_Concurrent(benchmark::State& state)
{
    static constexpr std::array<size_t,10> all_sizes = {8,16,32,64,128,256,512,1024,2048,4096};
    static default_slab* s = nullptr;
    if (state.thread_index() == 0) s = new default_slab();
    const size_t sz = all_sizes[static_cast<size_t>(state.thread_index()) % all_sizes.size()];
    for (auto _ : state)
    {
        void* p = s->palloc(sz);
        if (p) { benchmark::DoNotOptimize(p); s->free(p, sz); }
    }
    if (state.thread_index() == 0) { delete s; s = nullptr; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_TLC_Concurrent)->ThreadRange(1, 16);

static void BM_Slab_MultiSlab_NoChurn(benchmark::State& state)
{
    constexpr size_t num_slabs = slab_config<>::NUM_CACHED_SLABS;
    static std::vector<default_slab*>* slabs = nullptr;
    if (state.thread_index() == 0)
    {
        slabs = new std::vector<default_slab*>(num_slabs);
        for (auto& sp : *slabs) sp = new default_slab();
    }
    const size_t tid = static_cast<size_t>(state.thread_index());
    size_t i = 0;
    for (auto _ : state)
    {
        default_slab& sl = *(*slabs)[(tid + i) % num_slabs];
        size_t sz = (i % 2 == 0) ? 32 : 64;
        void* p = sl.palloc(sz);
        if (p) { benchmark::DoNotOptimize(p); sl.free(p, sz); }
        ++i;
    }
    if (state.thread_index() == 0)
    {
        for (auto* sp : *slabs) delete sp;
        delete slabs; slabs = nullptr;
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_MultiSlab_NoChurn)->ThreadRange(1, 16);

static void BM_Slab_MultiSlab_Churn(benchmark::State& state)
{
    constexpr size_t num_slabs = 2 * slab_config<>::NUM_CACHED_SLABS;
    static std::vector<default_slab*>* slabs = nullptr;
    if (state.thread_index() == 0)
    {
        slabs = new std::vector<default_slab*>(num_slabs);
        for (auto& sp : *slabs) sp = new default_slab();
    }
    const size_t tid = static_cast<size_t>(state.thread_index());
    size_t i = 0;
    for (auto _ : state)
    {
        default_slab& sl = *(*slabs)[(tid + i) % num_slabs];
        size_t sz = (i % 2 == 0) ? 32 : 64;
        void* p = sl.palloc(sz);
        if (p) { benchmark::DoNotOptimize(p); sl.free(p, sz); }
        ++i;
    }
    if (state.thread_index() == 0)
    {
        for (auto* sp : *slabs) delete sp;
        delete slabs; slabs = nullptr;
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_MultiSlab_Churn)->ThreadRange(1, 16);

BENCHMARK_MAIN();
