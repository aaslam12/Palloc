#include <benchmark/benchmark.h>
#include <benchmark/benchmark.h>
#include "slab.h"
#include <cstdlib>
#include <jemalloc/jemalloc.h>
#include <x86intrin.h>
#include <vector>
using namespace AL;

static constexpr size_t SIZES[] = {8, 16, 32, 64, 128, 256, 512, 1024};

static constexpr size_t RDTSC_BATCH = 1000;

static void BM_Slab_AllocFree(benchmark::State& state)
{
    const size_t sz = static_cast<size_t>(state.range(0));
    static default_slab* s = nullptr;
    if (state.thread_index() == 0) s = new default_slab();
    uint64_t cycles = 0;
    for (auto _ : state)
    {
        uint64_t t0 = __rdtsc();
        for (size_t i = 0; i < RDTSC_BATCH; ++i)
        {
            void* p = s->palloc(sz);
            benchmark::DoNotOptimize(p);
            s->free(p, sz);
        }
        cycles += __rdtsc() - t0;
    }
    if (state.thread_index() == 0) { delete s; s = nullptr; }
    state.SetItemsProcessed(state.iterations() * RDTSC_BATCH * 2);
    state.counters["cycles/op"] = (double)cycles / (double)(state.iterations() * RDTSC_BATCH);
}
BENCHMARK(BM_Slab_AllocFree)->Arg(8)->Arg(16)->Arg(32)->Arg(64)->Arg(128)->Arg(256)->Arg(512)->Arg(1024);

static void BM_Jemalloc_AllocFree(benchmark::State& state)
{
    const size_t sz = static_cast<size_t>(state.range(0));
    uint64_t cycles = 0;
    for (auto _ : state)
    {
        uint64_t t0 = __rdtsc();
        for (size_t i = 0; i < RDTSC_BATCH; ++i)
        {
            void* p = mallocx(sz, 0);
            benchmark::DoNotOptimize(p);
            dallocx(p, 0);
        }
        cycles += __rdtsc() - t0;
    }
    state.SetItemsProcessed(state.iterations() * RDTSC_BATCH * 2);
    state.counters["cycles/op"] = (double)cycles / (double)(state.iterations() * RDTSC_BATCH);
}
BENCHMARK(BM_Jemalloc_AllocFree)->Arg(8)->Arg(16)->Arg(32)->Arg(64)->Arg(128)->Arg(256)->Arg(512)->Arg(1024);

static void BM_Malloc_AllocFree(benchmark::State& state)
{
    const size_t sz = static_cast<size_t>(state.range(0));
    uint64_t cycles = 0;
    for (auto _ : state)
    {
        uint64_t t0 = __rdtsc();
        for (size_t i = 0; i < RDTSC_BATCH; ++i)
        {
            void* p = std::malloc(sz);
            benchmark::DoNotOptimize(p);
            std::free(p);
        }
        cycles += __rdtsc() - t0;
    }
    state.SetItemsProcessed(state.iterations() * RDTSC_BATCH * 2);
    state.counters["cycles/op"] = (double)cycles / (double)(state.iterations() * RDTSC_BATCH);
}
BENCHMARK(BM_Malloc_AllocFree)->Arg(8)->Arg(16)->Arg(32)->Arg(64)->Arg(128)->Arg(256)->Arg(512)->Arg(1024);

static void BM_Slab_Batch(benchmark::State& state)
{
    constexpr size_t batch = 256, sz = 64;
    static default_slab* s = nullptr;
    if (state.thread_index() == 0) s = new default_slab();
    void* ptrs[batch];
    for (auto _ : state)
    {
        for (size_t i = 0; i < batch; ++i) ptrs[i] = s->palloc(sz);
        for (size_t i = 0; i < batch; ++i) s->free(ptrs[i], sz);
    }
    if (state.thread_index() == 0) { delete s; s = nullptr; }
    state.SetItemsProcessed(state.iterations() * batch * 2);
}
BENCHMARK(BM_Slab_Batch);

static void BM_Jemalloc_Batch(benchmark::State& state)
{
    constexpr size_t batch = 256, sz = 64;
    void* ptrs[batch];
    for (auto _ : state)
    {
        for (size_t i = 0; i < batch; ++i) ptrs[i] = mallocx(sz, 0);
        for (size_t i = 0; i < batch; ++i) dallocx(ptrs[i], 0);
    }
    state.SetItemsProcessed(state.iterations() * batch * 2);
}
BENCHMARK(BM_Jemalloc_Batch);

static void BM_Malloc_Batch(benchmark::State& state)
{
    constexpr size_t batch = 256, sz = 64;
    void* ptrs[batch];
    for (auto _ : state)
    {
        for (size_t i = 0; i < batch; ++i) ptrs[i] = std::malloc(sz);
        for (size_t i = 0; i < batch; ++i) std::free(ptrs[i]);
    }
    state.SetItemsProcessed(state.iterations() * batch * 2);
}
BENCHMARK(BM_Malloc_Batch);

static void BM_Slab_MT(benchmark::State& state)
{
    static default_slab* s = nullptr;
    if (state.thread_index() == 0) s = new default_slab();
    for (auto _ : state) { void* p = s->palloc(32); benchmark::DoNotOptimize(p); if(p) s->free(p,32); }
    if (state.thread_index() == 0) { delete s; s = nullptr; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_MT)->ThreadRange(1, 8);

static void BM_Jemalloc_MT(benchmark::State& state)
{
    for (auto _ : state) { void* p = mallocx(32,0); benchmark::DoNotOptimize(p); dallocx(p,0); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Jemalloc_MT)->ThreadRange(1, 8);

static void BM_Malloc_MT(benchmark::State& state)
{
    for (auto _ : state) { void* p = std::malloc(32); benchmark::DoNotOptimize(p); std::free(p); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Malloc_MT)->ThreadRange(1, 8);

static void BM_Slab_Mixed(benchmark::State& state)
{
    static default_slab* s = nullptr;
    if (state.thread_index() == 0) s = new default_slab();
    size_t i = static_cast<size_t>(state.thread_index());
    for (auto _ : state) { size_t sz=SIZES[i%8]; void* p=s->palloc(sz); benchmark::DoNotOptimize(p); if(p) s->free(p,sz); ++i; }
    if (state.thread_index() == 0) { delete s; s = nullptr; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_Mixed)->ThreadRange(1, 8);

static void BM_Jemalloc_Mixed(benchmark::State& state)
{
    size_t i = 0;
    for (auto _ : state) { size_t sz=SIZES[i%8]; void* p=mallocx(sz,0); benchmark::DoNotOptimize(p); dallocx(p,0); ++i; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Jemalloc_Mixed)->ThreadRange(1, 8);

BENCHMARK_MAIN();
