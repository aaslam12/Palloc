#include <benchmark/benchmark.h>
#include "pool.h"
#include <cstring>
using namespace AL;

static void BM_Pool_Churn(benchmark::State& state)
{
    static pool<>* p = nullptr;
    if (state.thread_index() == 0)
        p = new pool<>(128, static_cast<size_t>(state.threads()) * 65536);
    for (auto _ : state)
    {
        void* ptr = p->alloc();
        if (ptr) { benchmark::DoNotOptimize(ptr); p->free(ptr); }
    }
    if (state.thread_index() == 0) { delete p; p = nullptr; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Pool_Churn)->ThreadRange(1, 16);

static void BM_Pool_ConcurrentReset(benchmark::State& state)
{
    static pool<>* p = nullptr;
    if (state.thread_index() == 0)
        p = new pool<>(96, static_cast<size_t>(state.threads()) * 131072);
    for (auto _ : state)
    {
        for (int i = 0; i < 256; ++i)
            benchmark::DoNotOptimize(p->alloc());
    }
    if (state.thread_index() == 0) { p->reset(); delete p; p = nullptr; }
    state.SetItemsProcessed(state.iterations() * 256);
}
BENCHMARK(BM_Pool_ConcurrentReset)->ThreadRange(1, 16)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
