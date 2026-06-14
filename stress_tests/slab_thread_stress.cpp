#include <benchmark/benchmark.h>
#include "slab.h"
#include <array>
#include <cstring>
using namespace AL;

static constexpr std::array<size_t, 18> REQUESTS = {
    1,8,9,16,17,32,33,64,65,128,129,256,512,1024,1025,2048,2049,4096};
static constexpr std::array<size_t, 10> SIZE_CLASSES = {8,16,32,64,128,256,512,1024,2048,4096};

static void BM_Slab_MixedContention(benchmark::State& state)
{
    static default_slab* s = nullptr;
    if (state.thread_index() == 0) s = new default_slab();
    size_t i = static_cast<size_t>(state.thread_index());
    for (auto _ : state)
    {
        size_t sz = REQUESTS[i % REQUESTS.size()];
        void* ptr = s->palloc(sz);
        if (ptr) { benchmark::DoNotOptimize(ptr); s->free(ptr, sz); }
        ++i;
    }
    if (state.thread_index() == 0) { delete s; s = nullptr; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_MixedContention)->ThreadRange(1, 16);

static void BM_Slab_PerClassContention(benchmark::State& state)
{
    static default_slab* s = nullptr;
    if (state.thread_index() == 0) s = new default_slab();
    const size_t sz = SIZE_CLASSES[static_cast<size_t>(state.thread_index()) % SIZE_CLASSES.size()];
    for (auto _ : state)
    {
        void* ptr = s->palloc(sz);
        if (ptr) { benchmark::DoNotOptimize(ptr); s->free(ptr, sz); }
    }
    if (state.thread_index() == 0) { delete s; s = nullptr; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_PerClassContention)->ThreadRange(1, 16);

BENCHMARK_MAIN();
