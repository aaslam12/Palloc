#include <benchmark/benchmark.h>
#include "arena.h"
#include <cstring>
using namespace AL;

static void BM_Arena_ConcurrentAlloc(benchmark::State& state)
{
    const size_t alloc_size = 32;
    const size_t allocs_per_thread = 10000;
    static arena<>* a = nullptr;
    if (state.thread_index() == 0)
        a = new arena<>(static_cast<size_t>(state.threads()) * allocs_per_thread * alloc_size);
    for (auto _ : state)
        benchmark::DoNotOptimize(a->alloc(alloc_size));
    if (state.thread_index() == 0) { delete a; a = nullptr; }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Arena_ConcurrentAlloc)->ThreadRange(1, 16);

static void BM_Arena_ResetCycle(benchmark::State& state)
{
    const size_t alloc_size = 32;
    const size_t allocs = 50000;
    static arena<>* a = nullptr;
    if (state.thread_index() == 0)
        a = new arena<>(static_cast<size_t>(state.threads()) * allocs * alloc_size);
    for (auto _ : state)
        for (size_t i = 0; i < allocs; ++i)
            benchmark::DoNotOptimize(a->alloc(alloc_size));
    if (state.thread_index() == 0) { a->reset(); delete a; a = nullptr; }
    state.SetItemsProcessed(state.iterations() * allocs);
}
BENCHMARK(BM_Arena_ResetCycle)->ThreadRange(1, 16)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
