// Comprehensive benchmark: Palloc (Arena, Pool, Slab, Dynamic Slab) vs jemalloc vs glibc malloc

#include <benchmark/benchmark.h>
#include "arena.h"
#include "pool.h"
#include "slab.h"
#include <atomic>
#include <cstdlib>
#include <jemalloc/jemalloc.h>
#include <thread>
#include <vector>

using namespace AL;

static void BM_Slab_AllocFree(benchmark::State& state)
{
    const size_t sz = static_cast<size_t>(state.range(0));
    static default_slab* s = nullptr;
    if (state.thread_index() == 0) s = new default_slab();
    for (auto _ : state) { void* p = s->palloc(sz); benchmark::DoNotOptimize(p); s->free(p, sz); }
    if (state.thread_index() == 0) { delete s; s = nullptr; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_AllocFree)->Arg(8)->Arg(16)->Arg(32)->Arg(64)->Arg(128)->Arg(256)->Arg(512)->Arg(1024)->Arg(2048)->Arg(4096);


static void BM_Jemalloc_AllocFree(benchmark::State& state)
{
    const size_t sz = static_cast<size_t>(state.range(0));
    for (auto _ : state) { void* p = mallocx(sz, 0); benchmark::DoNotOptimize(p); dallocx(p, 0); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Jemalloc_AllocFree)->Arg(8)->Arg(16)->Arg(32)->Arg(64)->Arg(128)->Arg(256)->Arg(512)->Arg(1024)->Arg(2048)->Arg(4096);

static void BM_Malloc_AllocFree(benchmark::State& state)
{
    const size_t sz = static_cast<size_t>(state.range(0));
    for (auto _ : state) { void* p = std::malloc(sz); benchmark::DoNotOptimize(p); std::free(p); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Malloc_AllocFree)->Arg(8)->Arg(16)->Arg(32)->Arg(64)->Arg(128)->Arg(256)->Arg(512)->Arg(1024)->Arg(2048)->Arg(4096);

static void BM_Arena_LinearAlloc(benchmark::State& state)
{
    constexpr size_t sz = 64, ops = 1000000;
    for (auto _ : state)
    {
        state.PauseTiming(); arena<> a(ops * sz); state.ResumeTiming();
        for (size_t i = 0; i < ops; ++i) benchmark::DoNotOptimize(a.alloc(sz));
    }
    state.SetItemsProcessed(state.iterations() * ops);
}
BENCHMARK(BM_Arena_LinearAlloc)->Unit(benchmark::kMillisecond);

static void BM_Pool_LinearAlloc(benchmark::State& state)
{
    constexpr size_t sz = 64, ops = 1000000;
    for (auto _ : state)
    {
        state.PauseTiming(); pool<> p(sz, ops); state.ResumeTiming();
        for (size_t i = 0; i < ops; ++i) benchmark::DoNotOptimize(p.alloc());
    }
    state.SetItemsProcessed(state.iterations() * ops);
}
BENCHMARK(BM_Pool_LinearAlloc)->Unit(benchmark::kMillisecond);

static void BM_Pool_Fixed64(benchmark::State& state)
{
    pool<> p(64, 1000000);
    for (auto _ : state) { void* ptr = p.alloc(); benchmark::DoNotOptimize(ptr); p.free(ptr); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Pool_Fixed64);

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
BENCHMARK(BM_Slab_MT)->ThreadRange(1, 16);

static void BM_Jemalloc_MT(benchmark::State& state)
{
    for (auto _ : state) { void* p = mallocx(32,0); benchmark::DoNotOptimize(p); dallocx(p,0); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Jemalloc_MT)->ThreadRange(1, 16);

static void BM_Malloc_MT(benchmark::State& state)
{
    for (auto _ : state) { void* p = std::malloc(32); benchmark::DoNotOptimize(p); std::free(p); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Malloc_MT)->ThreadRange(1, 16);

static constexpr size_t SHOW_SIZES[] = {8,16,32,64,128,256,512,1024,2048,4096};
static void BM_Slab_Mixed(benchmark::State& state)
{
    static default_slab* s = nullptr;
    if (state.thread_index() == 0) s = new default_slab();
    size_t i = static_cast<size_t>(state.thread_index());
    for (auto _ : state) { size_t sz=SHOW_SIZES[i%10]; void* p=s->palloc(sz); benchmark::DoNotOptimize(p); if(p) s->free(p,sz); ++i; }
    if (state.thread_index() == 0) { delete s; s = nullptr; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_Mixed)->ThreadRange(1, 16);

static void BM_Jemalloc_Mixed(benchmark::State& state)
{
    size_t i = 0;
    for (auto _ : state) { size_t sz=SHOW_SIZES[i%10]; void* p=mallocx(sz,0); benchmark::DoNotOptimize(p); dallocx(p,0); ++i; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Jemalloc_Mixed)->ThreadRange(1, 16);

static void BM_Malloc_Mixed(benchmark::State& state)
{
    size_t i = 0;
    for (auto _ : state) { size_t sz=SHOW_SIZES[i%10]; void* p=std::malloc(sz); benchmark::DoNotOptimize(p); std::free(p); ++i; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Malloc_Mixed)->ThreadRange(1, 16);

static void BM_Slab_Calloc(benchmark::State& state)
{
    const size_t sz = static_cast<size_t>(state.range(0));
    static default_slab* s = nullptr;
    if (state.thread_index() == 0) s = new default_slab();
    for (auto _ : state) { void* p = s->calloc(sz); benchmark::DoNotOptimize(p); s->free(p, sz); }
    if (state.thread_index() == 0) { delete s; s = nullptr; }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Slab_Calloc)->Arg(32)->Arg(256)->Arg(1024)->Arg(4096);

static void BM_Jemalloc_Calloc(benchmark::State& state)
{
    const size_t sz = static_cast<size_t>(state.range(0));
    for (auto _ : state) { void* p = mallocx(sz, MALLOCX_ZERO); benchmark::DoNotOptimize(p); dallocx(p, 0); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Jemalloc_Calloc)->Arg(32)->Arg(256)->Arg(1024)->Arg(4096);

static void BM_Malloc_Calloc(benchmark::State& state)
{
    const size_t sz = static_cast<size_t>(state.range(0));
    for (auto _ : state) { void* p = std::calloc(1, sz); benchmark::DoNotOptimize(p); std::free(p); }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Malloc_Calloc)->Arg(32)->Arg(256)->Arg(1024)->Arg(4096);

BENCHMARK_MAIN();
