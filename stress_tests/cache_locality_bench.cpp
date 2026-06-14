// ═══════════════════════════════════════════════════════════════════════════════
// Cache Locality Benchmark
//
// Measures the cache performance of pre-allocated object pools vs malloc.
// In HFT the allocator is not on the hot path — objects are pre-allocated
// at startup and recycled. What matters is whether the allocator's memory
// layout gives better cache locality during sequential traversal.
//
// Tests:
//   1. Sequential read: iterate all live objects, sum a field
//   2. Sequential write: iterate all live objects, update a field
//   3. Random access: access objects in random order (TLB/cache miss stress)
//   4. Pointer-chase: linked list traversal (realistic order book pattern)
//
// Allocators: slab (contiguous mmap), malloc (scattered heap)
// ═══════════════════════════════════════════════════════════════════════════════

#include <benchmark/benchmark.h>
#include "slab.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

// ─── Object: 64 bytes, matches a typical order/message struct ────────────────

struct alignas(64) Object
{
    uint64_t id;
    uint64_t value;
    uint64_t timestamp;
    uint64_t checksum;
    Object*  next;       // for linked list traversal
    uint8_t  padding[24];
};
static_assert(sizeof(Object) == 64);

// ─── Slab config: single 64B class ───────────────────────────────────────────

constexpr std::array<AL::size_class, 1> obj_classes = {
    AL::size_class{.byte_size = 64, .num_blocks = 60'000, .batch_size = 128}};
using obj_slab_cfg = AL::slab_config<1, obj_classes>;

static constexpr size_t N          = 50'000; // live objects
static constexpr size_t ITERATIONS = 5;      // traversal passes
static constexpr size_t WARMUP     = 1;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static uint64_t prevent_opt = 0;

// Sequential read: sum all values (pointer array)
static uint64_t seq_read(Object** objs, size_t n)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i) [[likely]]
    {
        __builtin_prefetch(objs[i + 8], 0, 1);
        sum += objs[i]->value;
    }
    return sum;
}

// Sequential write: update all values (pointer array)
static void seq_write(Object** objs, size_t n, uint64_t v)
{
    for (size_t i = 0; i < n; ++i) [[likely]]
    {
        __builtin_prefetch(objs[i + 8], 1, 1);
        objs[i]->value = v;
    }
}

// Random access: access in shuffled order
static uint64_t rand_read(Object** objs, size_t* perm, size_t n)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i) [[likely]]
    {
        __builtin_prefetch(objs[perm[i + 8]], 0, 0);
        sum += objs[perm[i]]->value;
    }
    return sum;
}

// Pointer chase: follow linked list with deep prefetch pipeline
static uint64_t ptr_chase(Object* head)
{
    constexpr int DEPTH = 8;
    // prime the prefetch pipeline
    Object* pre = head;
    for (int i = 0; i < DEPTH && pre; ++i)
    {
        __builtin_prefetch(pre, 0, 1);
        pre = pre->next;
    }

    uint64_t sum = 0;
    for (Object* p = head; p != nullptr; p = p->next) [[likely]]
    {
        if (pre) [[likely]]
        {
            __builtin_prefetch(pre, 0, 1);
            pre = pre->next;
        }
        sum += p->value;
    }
    return sum;
}

// ── flat array variants (slab only — contiguous region) ──────────────────────

static uint64_t seq_read_flat(Object* base, size_t n)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i) [[likely]]
    {
        __builtin_prefetch(&base[i + 8], 0, 1);
        sum += base[i].value;
    }
    return sum;
}

static void seq_write_flat(Object* base, size_t n, uint64_t v)
{
    for (size_t i = 0; i < n; ++i) [[likely]]
    {
        __builtin_prefetch(&base[i + 8], 1, 1);
        base[i].value = v;
    }
}

static uint64_t rand_read_flat(Object* base, size_t* perm, size_t n)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i) [[likely]]
    {
        __builtin_prefetch(&base[perm[i + 8]], 0, 0);
        sum += base[perm[i]].value;
    }
    return sum;
}

// ─── Run one allocator ────────────────────────────────────────────────────────

struct Result
{
    double seq_read_ns;
    double seq_write_ns;
    double rand_read_ns;
    double ptr_chase_ns;
};

template<typename AllocFn, typename FreeFn>
static Result run(AllocFn alloc, FreeFn free_fn, size_t* perm)
{
    // allocate
    std::vector<Object*> objs(N + 16); // +16 for prefetch overread safety
    for (size_t i = 0; i < N; ++i)
    {
        objs[i] = static_cast<Object*>(alloc());
        objs[i]->id = i;
        objs[i]->value = i * 3 + 7;
        objs[i]->timestamp = i;
        objs[i]->checksum = 0;
        objs[i]->next = nullptr;
    }
    // fill safety slots so prefetch[i+8] never reads garbage
    for (size_t i = N; i < N + 16; ++i) objs[i] = objs[N - 1];

    // build linked list in allocation order
    for (size_t i = 0; i + 1 < N; ++i)
        objs[i]->next = objs[i + 1];

    // warmup
    for (size_t w = 0; w < WARMUP; ++w)
    {
        prevent_opt += seq_read(objs.data(), N);
        seq_write(objs.data(), N, w);
    }

    uint64_t t0;

    t0 = __builtin_ia32_rdtsc();
    for (size_t it = 0; it < ITERATIONS; ++it)
        prevent_opt += seq_read(objs.data(), N);
    uint64_t seq_r_cycles = __builtin_ia32_rdtsc() - t0;

    t0 = __builtin_ia32_rdtsc();
    for (size_t it = 0; it < ITERATIONS; ++it)
        seq_write(objs.data(), N, it);
    uint64_t seq_w_cycles = __builtin_ia32_rdtsc() - t0;
    prevent_opt += objs[0]->value;

    t0 = __builtin_ia32_rdtsc();
    for (size_t it = 0; it < ITERATIONS; ++it)
        prevent_opt += rand_read(objs.data(), perm, N);
    uint64_t rand_r_cycles = __builtin_ia32_rdtsc() - t0;

    t0 = __builtin_ia32_rdtsc();
    for (size_t it = 0; it < ITERATIONS; ++it)
        prevent_opt += ptr_chase(objs[0]);
    uint64_t ptr_cycles = __builtin_ia32_rdtsc() - t0;

    for (size_t i = 0; i < N; ++i)
        free_fn(objs[i]);

    size_t total_ops = N * ITERATIONS;
    auto to_ns = [&](uint64_t c) { return (double)c / (double)total_ops / 3.5; };
    return {to_ns(seq_r_cycles), to_ns(seq_w_cycles),
            to_ns(rand_r_cycles), to_ns(ptr_cycles)};
}

// ─── Benchmarks ──────────────────────────────────────────────────────────────

static void BM_CacheLocality_Slab_PtrArray(benchmark::State& state)
{
    std::vector<size_t> perm(N + 16, 0);
    std::iota(perm.begin(), perm.begin() + N, 0);
    std::mt19937_64 rng(42);
    std::shuffle(perm.begin(), perm.begin() + N, rng);

    AL::slab<obj_slab_cfg> s;
    auto slab_alloc = [&] { return s.palloc(64); };
    auto slab_free  = [&](Object* p) { s.free(p, 64); };

    for (auto _ : state)
    {
        Result r = run(slab_alloc, slab_free, perm.data());
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_CacheLocality_Slab_PtrArray)->Unit(benchmark::kMillisecond);

static void BM_CacheLocality_Malloc(benchmark::State& state)
{
    std::vector<size_t> perm(N + 16, 0);
    std::iota(perm.begin(), perm.begin() + N, 0);
    std::mt19937_64 rng(42);
    std::shuffle(perm.begin(), perm.begin() + N, rng);

    auto mal_alloc = [] { return std::malloc(sizeof(Object)); };
    auto mal_free  = [](Object* p) { std::free(p); };

    for (auto _ : state)
    {
        Result r = run(mal_alloc, mal_free, perm.data());
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_CacheLocality_Malloc)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
