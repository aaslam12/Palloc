#include <benchmark/benchmark.h>
#include "arena.h"
#include <unistd.h>
using namespace AL;

static void BM_Arena_SmallAllocs(benchmark::State& state)
{
    const size_t page_size = static_cast<size_t>(getpagesize());
    const int allocs = 200000;
    for (auto _ : state)
    {
        state.PauseTiming();
        arena<> a(page_size * 1000);
        state.ResumeTiming();
        for (int i = 0; i < allocs; ++i)
            benchmark::DoNotOptimize(a.alloc(8));
    }
    state.SetItemsProcessed(state.iterations() * allocs);
}
BENCHMARK(BM_Arena_SmallAllocs)->Unit(benchmark::kMillisecond);

static void BM_Arena_ResetCycle(benchmark::State& state)
{
    const size_t page_size = static_cast<size_t>(getpagesize());
    arena<> a(page_size * 32);
    for (auto _ : state)
    {
        for (int i = 0; i < 1000; ++i)
            benchmark::DoNotOptimize(a.alloc(100));
        a.reset();
    }
    state.SetItemsProcessed(state.iterations() * 1001);
}
BENCHMARK(BM_Arena_ResetCycle);
