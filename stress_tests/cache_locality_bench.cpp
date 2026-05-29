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

#include "low_overhead_bench.h"
#include "slab.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

using namespace bench;

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
    AL::size_class{.byte_size = 64, .num_blocks = 5'500'000, .batch_size = 128}};
using obj_slab_cfg = AL::slab_config<1, obj_classes>;

static constexpr size_t N          = 5'000'000; // live objects
static constexpr size_t ITERATIONS = 20;        // traversal passes (5M*20 = 100M ops)
static constexpr size_t WARMUP     = 2;

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

    t0 = rdtsc();
    for (size_t it = 0; it < ITERATIONS; ++it)
        prevent_opt += seq_read(objs.data(), N);
    uint64_t seq_r_cycles = rdtsc() - t0;

    t0 = rdtsc();
    for (size_t it = 0; it < ITERATIONS; ++it)
        seq_write(objs.data(), N, it);
    uint64_t seq_w_cycles = rdtsc() - t0;
    prevent_opt += objs[0]->value;

    t0 = rdtsc();
    for (size_t it = 0; it < ITERATIONS; ++it)
        prevent_opt += rand_read(objs.data(), perm, N);
    uint64_t rand_r_cycles = rdtsc() - t0;

    t0 = rdtsc();
    for (size_t it = 0; it < ITERATIONS; ++it)
        prevent_opt += ptr_chase(objs[0]);
    uint64_t ptr_cycles = rdtsc() - t0;

    for (size_t i = 0; i < N; ++i)
        free_fn(objs[i]);

    size_t total_ops = N * ITERATIONS;
    auto to_ns = [&](uint64_t c) { return (double)c / (double)total_ops / 3.5; };
    return {to_ns(seq_r_cycles), to_ns(seq_w_cycles),
            to_ns(rand_r_cycles), to_ns(ptr_cycles)};
}

// flat-array variant: slab only, direct index into contiguous region
static Result run_flat(Object* base, size_t* perm)
{
    // init
    for (size_t i = 0; i < N; ++i)
    {
        base[i].id = i; base[i].value = i * 3 + 7;
        base[i].timestamp = i; base[i].checksum = 0;
        base[i].next = (i + 1 < N) ? &base[i + 1] : nullptr;
    }

    // warmup
    for (size_t w = 0; w < WARMUP; ++w)
    {
        prevent_opt += seq_read_flat(base, N);
        seq_write_flat(base, N, w);
    }

    uint64_t t0;
    // perm safety: perm values are in [0,N), prefetch perm[i+8] needs N+8 entries
    // perm was built for N elements; last 8 entries wrap to 0 safely via modulo
    std::vector<size_t> safe_perm(perm, perm + N + 16);
    for (size_t i = N; i < N + 16; ++i) safe_perm[i] = 0;

    t0 = rdtsc();
    for (size_t it = 0; it < ITERATIONS; ++it)
        prevent_opt += seq_read_flat(base, N);
    uint64_t seq_r = rdtsc() - t0;

    t0 = rdtsc();
    for (size_t it = 0; it < ITERATIONS; ++it)
        seq_write_flat(base, N, it);
    uint64_t seq_w = rdtsc() - t0;
    prevent_opt += base[0].value;

    t0 = rdtsc();
    for (size_t it = 0; it < ITERATIONS; ++it)
        prevent_opt += rand_read_flat(base, safe_perm.data(), N);
    uint64_t rand_r = rdtsc() - t0;

    // pointer chase same as before (linked list through flat array)
    t0 = rdtsc();
    for (size_t it = 0; it < ITERATIONS; ++it)
        prevent_opt += ptr_chase(&base[0]);
    uint64_t ptr_c = rdtsc() - t0;

    size_t total_ops = N * ITERATIONS;
    auto to_ns = [&](uint64_t c) { return (double)c / (double)total_ops / 3.5; };
    return {to_ns(seq_r), to_ns(seq_w), to_ns(rand_r), to_ns(ptr_c)};
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main()
{
    std::printf("=== Cache Locality Benchmark ===\n");
    std::printf("Objects: %zu x %zu bytes  |  Passes: %zu\n\n", N, sizeof(Object), ITERATIONS);

    // permutation for random access
    std::vector<size_t> perm(N);
    std::iota(perm.begin(), perm.end(), 0);
    std::mt19937_64 rng(42);
    std::shuffle(perm.begin(), perm.end(), rng);

    // ── slab (pointer array) ──
    AL::slab<obj_slab_cfg> s;
    auto slab_alloc = [&] { return s.palloc(64); };
    auto slab_free  = [&](Object* p) { s.free(p, 64); };
    Result sr = run(slab_alloc, slab_free, perm.data());

    // ── slab (flat array — direct region access, no pointer indirection) ──
    Object* base = reinterpret_cast<Object*>(s.region_start());
    Result sf = run_flat(base, perm.data());

    // ── malloc ──
    auto mal_alloc = [] { return std::malloc(sizeof(Object)); };
    auto mal_free  = [](Object* p) { std::free(p); };
    Result mr = run(mal_alloc, mal_free, perm.data());

    // ── print ──
    auto pct = [](double base_v, double val) {
        return (base_v - val) / base_v * 100.0; // positive = val faster
    };

    std::printf("%-28s %10s %10s %10s %10s\n",
                "Allocator", "seq-read", "seq-write", "rand-read", "ptr-chase");
    std::printf("%-28s %10s %10s %10s %10s\n",
                "", "(ns/obj)", "(ns/obj)", "(ns/obj)", "(ns/obj)");
    std::printf("%s\n", std::string(70, '-').c_str());
    std::printf("%-28s %10.2f %10.2f %10.2f %10.2f\n",
                "slab (ptr array)", sr.seq_read_ns, sr.seq_write_ns, sr.rand_read_ns, sr.ptr_chase_ns);
    std::printf("%-28s %10.2f %10.2f %10.2f %10.2f\n",
                "slab (flat array)", sf.seq_read_ns, sf.seq_write_ns, sf.rand_read_ns, sf.ptr_chase_ns);
    std::printf("%-28s %10.2f %10.2f %10.2f %10.2f\n",
                "malloc (ptr array)", mr.seq_read_ns, mr.seq_write_ns, mr.rand_read_ns, mr.ptr_chase_ns);
    std::printf("%s\n", std::string(70, '-').c_str());
    std::printf("%-28s %+9.1f%% %+9.1f%% %+9.1f%% %+9.1f%%\n",
                "slab-ptr vs malloc",
                pct(mr.seq_read_ns,  sr.seq_read_ns),
                pct(mr.seq_write_ns, sr.seq_write_ns),
                pct(mr.rand_read_ns, sr.rand_read_ns),
                pct(mr.ptr_chase_ns, sr.ptr_chase_ns));
    std::printf("%-28s %+9.1f%% %+9.1f%% %+9.1f%% %+9.1f%%\n",
                "slab-flat vs malloc",
                pct(mr.seq_read_ns,  sf.seq_read_ns),
                pct(mr.seq_write_ns, sf.seq_write_ns),
                pct(mr.rand_read_ns, sf.rand_read_ns),
                pct(mr.ptr_chase_ns, sf.ptr_chase_ns));
    std::printf("%-28s %+9.1f%% %+9.1f%% %+9.1f%% %+9.1f%%\n",
                "slab-flat vs slab-ptr",
                pct(sr.seq_read_ns,  sf.seq_read_ns),
                pct(sr.seq_write_ns, sf.seq_write_ns),
                pct(sr.rand_read_ns, sf.rand_read_ns),
                pct(sr.ptr_chase_ns, sf.ptr_chase_ns));

    std::printf("\n(positive %% = slab faster)\n");
    std::printf("prevent_opt=%llu\n", (unsigned long long)prevent_opt);
    return 0;
}
