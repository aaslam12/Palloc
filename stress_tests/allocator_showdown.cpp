// Comprehensive benchmark: Palloc (Arena, Pool, Slab, Dynamic Slab) vs jemalloc vs glibc malloc
// Uses volatile sink + asm clobber to prevent compiler from optimizing away allocations.
// Reports results in CPU cycles (RDTSC) for deterministic, low-overhead measurement.

#include "arena.h"
#include "dynamic_slab.h"
#include "low_overhead_bench.h"
#include "pool.h"
#include "slab.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#include <jemalloc/jemalloc.h>

using namespace AL;
using namespace bench;

namespace
{

struct result
{
    const char* label;
    double cycles;
};

void print_results(std::vector<result>& results)
{
    std::sort(results.begin(), results.end(), [](const result& a, const result& b) { return a.cycles < b.cycles; });
    std::cout << "  " << std::left << std::setw(20) << "Allocator" << std::right << std::setw(12) << "cycles/op\n";
    std::cout << "  " << std::string(32, '-') << "\n";
    for (const auto& r : results)
        std::cout << "  " << std::left << std::setw(20) << r.label
                  << std::right << std::setw(10) << std::fixed << std::setprecision(1) << r.cycles << "\n";
    std::cout << "\n";
}

// Returns cycles/op over ops*2 (alloc+free)
template <typename F>
double bench_alloc_free(F fn, size_t ops)
{
    uint64_t t0 = rdtsc();
    fn(ops);
    return cycles_per_op(rdtsc() - t0, ops * 2);
}

// Returns cycles/op for alloc-only
template <typename F>
double bench_alloc_only(F fn, size_t ops)
{
    uint64_t t0 = rdtsc();
    fn(ops);
    return cycles_per_op(rdtsc() - t0, ops);
}

} // namespace

int main()
{
    const size_t threads = worker_count();

    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║     Allocator Showdown: Palloc vs jemalloc vs malloc     ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Threads: " << std::left << std::setw(47) << threads << "║\n";
    std::cout << "║  Timing:  RDTSC (cycles/op)                              ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";

    // ─────────────────────────────────────────────────────────────────────────
    // Test 1: Single-threaded alloc+free per size class
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 1: Single-threaded alloc+free (1M ops per size) ━━━\n\n";
        constexpr size_t ops = 1'000'000;
        constexpr size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

        std::cout << "  " << std::left << std::setw(8) << "Size"
                  << std::right << std::setw(10) << "Slab" << std::setw(10) << "DynSlab"
                  << std::setw(10) << "jemalloc" << std::setw(10) << "malloc"
                  << "  (cycles/op)\n";
        std::cout << "  " << std::string(48, '-') << "\n";

        for (size_t sz : sizes)
        {
            default_slab ps{};
            uint64_t t0 = rdtsc();
            for (size_t i = 0; i < ops; ++i) { void* p = ps.alloc(sz); escape(p); ps.free(p, sz); clobber(); }
            double slab = cycles_per_op(rdtsc() - t0, ops * 2);

            default_dynamic_slab ds{};
            t0 = rdtsc();
            for (size_t i = 0; i < ops; ++i) { void* p = ds.palloc(sz); escape(p); ds.free(p, sz); clobber(); }
            double dslab = cycles_per_op(rdtsc() - t0, ops * 2);

            t0 = rdtsc();
            for (size_t i = 0; i < ops; ++i) { void* p = mallocx(sz, 0); escape(p); dallocx(p, 0); clobber(); }
            double je = cycles_per_op(rdtsc() - t0, ops * 2);

            t0 = rdtsc();
            for (size_t i = 0; i < ops; ++i) { void* p = std::malloc(sz); escape(p); std::free(p); clobber(); }
            double mal = cycles_per_op(rdtsc() - t0, ops * 2);

            char line[128];
            std::snprintf(line, sizeof(line), "  %4zuB   %7.1f   %7.1f   %7.1f   %7.1f", sz, slab, dslab, je, mal);
            std::cout << line << "\n";
        }
        std::cout << "\n";
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Test 2: Linear allocation (alloc only, no free)
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 2: Linear allocation (no free, 64B, 1M ops) ━━━\n\n";
        constexpr size_t ops = 1'000'000;
        constexpr size_t sz = 64;
        std::vector<result> results;

        arena a(ops * sz);
        uint64_t t0 = rdtsc();
        for (size_t i = 0; i < ops; ++i) { void* p = a.alloc(sz); escape(p); }
        results.push_back({"Arena", cycles_per_op(rdtsc() - t0, ops)});

        pool po(sz, ops);
        t0 = rdtsc();
        for (size_t i = 0; i < ops; ++i) { void* p = po.alloc(); escape(p); }
        results.push_back({"Pool", cycles_per_op(rdtsc() - t0, ops)});

        std::vector<void*> ptrs(ops);
        t0 = rdtsc();
        for (size_t i = 0; i < ops; ++i) { ptrs[i] = mallocx(sz, 0); escape(ptrs[i]); }
        results.push_back({"jemalloc", cycles_per_op(rdtsc() - t0, ops)});
        for (auto* p : ptrs) dallocx(p, 0);

        t0 = rdtsc();
        for (size_t i = 0; i < ops; ++i) { ptrs[i] = std::malloc(sz); escape(ptrs[i]); }
        results.push_back({"malloc", cycles_per_op(rdtsc() - t0, ops)});
        for (auto* p : ptrs) std::free(p);

        print_results(results);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Test 3: Fixed-size alloc+free
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 3: Fixed-size alloc+free (64B, 1M ops) ━━━\n\n";
        constexpr size_t ops = 1'000'000;
        constexpr size_t sz = 64;
        std::vector<result> results;

        pool po(sz, ops);
        uint64_t t0 = rdtsc();
        for (size_t i = 0; i < ops; ++i) { void* p = po.alloc(); escape(p); po.free(p); clobber(); }
        results.push_back({"Pool", cycles_per_op(rdtsc() - t0, ops * 2)});

        default_slab s{};
        t0 = rdtsc();
        for (size_t i = 0; i < ops; ++i) { void* p = s.alloc(sz); escape(p); s.free(p, sz); clobber(); }
        results.push_back({"Slab (TLC)", cycles_per_op(rdtsc() - t0, ops * 2)});

        t0 = rdtsc();
        for (size_t i = 0; i < ops; ++i) { void* p = mallocx(sz, 0); escape(p); dallocx(p, 0); clobber(); }
        results.push_back({"jemalloc", cycles_per_op(rdtsc() - t0, ops * 2)});

        t0 = rdtsc();
        for (size_t i = 0; i < ops; ++i) { void* p = std::malloc(sz); escape(p); std::free(p); clobber(); }
        results.push_back({"malloc", cycles_per_op(rdtsc() - t0, ops * 2)});

        print_results(results);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Test 4: Batch alloc-then-free
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 4: Batch alloc-then-free (256 objects × 200K cycles, 64B) ━━━\n\n";
        constexpr size_t batch = 256;
        constexpr size_t ncycles = 200'000;
        constexpr size_t sz = 64;
        std::vector<void*> ptrs(batch);
        std::vector<result> results;

        auto run_batch = [&](auto alloc_fn, auto free_fn) -> double {
            uint64_t t0 = rdtsc();
            for (size_t c = 0; c < ncycles; ++c)
            {
                for (size_t i = 0; i < batch; ++i) { ptrs[i] = alloc_fn(); escape(ptrs[i]); }
                for (size_t i = 0; i < batch; ++i) { free_fn(ptrs[i]); clobber(); }
            }
            return cycles_per_op(rdtsc() - t0, ncycles * batch * 2);
        };

        default_slab ps{};
        results.push_back({"Slab (TLC)", run_batch([&] { return ps.alloc(sz); }, [&](void* p) { ps.free(p, sz); })});

        default_dynamic_slab ds{};
        results.push_back({"Dynamic Slab", run_batch([&] { return ds.palloc(sz); }, [&](void* p) { ds.free(p, sz); })});

        results.push_back({"jemalloc", run_batch([] { return mallocx(sz, 0); }, [](void* p) { dallocx(p, 0); })});
        results.push_back({"malloc",   run_batch([] { return std::malloc(sz); }, [](void* p) { std::free(p); })});

        print_results(results);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Test 5: Multi-threaded alloc+free — single size
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 5: Multi-threaded alloc+free (" << threads << " threads, 32B, 500K iters) ━━━\n\n";
        constexpr size_t iters = 500'000;
        constexpr size_t sz = 32;
        std::vector<result> results;

        auto run_mt = [&](const char* label, auto alloc_fn, auto free_fn) {
            std::atomic<bool> go{false};
            std::atomic<size_t> total_ops{0};
            std::vector<std::thread> workers;
            workers.reserve(threads);

            uint64_t t0 = rdtsc();
            for (size_t i = 0; i < threads; ++i)
            {
                workers.emplace_back([&] {
                    size_t local = 0;
                    wait_for_start(go);
                    for (size_t j = 0; j < iters; ++j)
                    {
                        void* p = alloc_fn();
                        escape(p);
                        if (p) { free_fn(p); clobber(); local += 2; }
                    }
                    total_ops.fetch_add(local, std::memory_order_relaxed);
                });
            }
            go.store(true, std::memory_order_release);
            for (auto& t : workers) t.join();
            results.push_back({label, cycles_per_op(rdtsc() - t0, total_ops.load())});
        };

        default_slab ps{};
        run_mt("Slab (TLC)",   [&] { return ps.alloc(sz); }, [&](void* p) { ps.free(p, sz); });
        default_dynamic_slab ds{};
        run_mt("Dynamic Slab", [&] { return ds.palloc(sz); }, [&](void* p) { ds.free(p, sz); });
        run_mt("jemalloc",     [] { return mallocx(sz, 0); }, [](void* p) { dallocx(p, 0); });
        run_mt("malloc",       [] { return std::malloc(sz); }, [](void* p) { std::free(p); });

        print_results(results);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Test 6: Multi-threaded mixed sizes
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 6: Multi-threaded mixed sizes (" << threads << " threads, 300K iters) ━━━\n\n";
        constexpr size_t iters = 300'000;
        constexpr size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
        std::vector<result> results;

        auto run_mixed = [&](const char* label, auto alloc_fn, auto free_fn) {
            std::atomic<bool> go{false};
            std::atomic<size_t> total_ops{0};
            std::vector<std::thread> workers;
            workers.reserve(threads);

            uint64_t t0 = rdtsc();
            for (size_t tid = 0; tid < threads; ++tid)
            {
                workers.emplace_back([&, tid] {
                    size_t local = 0;
                    wait_for_start(go);
                    for (size_t i = 0; i < iters; ++i)
                    {
                        size_t sz = sizes[(tid + i) % 10];
                        void* p = alloc_fn(sz);
                        escape(p);
                        if (p) { free_fn(p, sz); clobber(); local += 2; }
                    }
                    total_ops.fetch_add(local, std::memory_order_relaxed);
                });
            }
            go.store(true, std::memory_order_release);
            for (auto& t : workers) t.join();
            results.push_back({label, cycles_per_op(rdtsc() - t0, total_ops.load())});
        };

        default_slab ps{};
        run_mixed("Slab (TLC)",   [&](size_t sz) { return ps.alloc(sz); },   [&](void* p, size_t sz) { ps.free(p, sz); });
        default_dynamic_slab ds{};
        run_mixed("Dynamic Slab", [&](size_t sz) { return ds.palloc(sz); },  [&](void* p, size_t sz) { ds.free(p, sz); });
        run_mixed("jemalloc",     [](size_t sz) { return mallocx(sz, 0); },  [](void* p, size_t) { dallocx(p, 0); });
        run_mixed("malloc",       [](size_t sz) { return std::malloc(sz); }, [](void* p, size_t) { std::free(p); });

        print_results(results);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Test 7: Multi-threaded batch hold (long-lived objects)
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 7: MT batch hold (" << threads << " threads, hold 500, 100 cycles) ━━━\n\n";
        constexpr size_t hold = 500;
        constexpr size_t ncycles = 100;
        constexpr size_t sz = 64;
        std::vector<result> results;

        auto run_hold = [&](const char* label, auto alloc_fn, auto free_fn) {
            std::atomic<bool> go{false};
            std::atomic<size_t> total_ops{0};
            std::vector<std::thread> workers;
            workers.reserve(threads);

            uint64_t t0 = rdtsc();
            for (size_t i = 0; i < threads; ++i)
            {
                workers.emplace_back([&] {
                    std::vector<void*> ptrs(hold);
                    size_t local = 0;
                    wait_for_start(go);
                    for (size_t c = 0; c < ncycles; ++c)
                    {
                        for (size_t j = 0; j < hold; ++j) { ptrs[j] = alloc_fn(); escape(ptrs[j]); }
                        for (size_t j = 0; j < hold; ++j) { if (ptrs[j]) { free_fn(ptrs[j]); clobber(); } }
                        local += hold * 2;
                    }
                    total_ops.fetch_add(local, std::memory_order_relaxed);
                });
            }
            go.store(true, std::memory_order_release);
            for (auto& t : workers) t.join();
            results.push_back({label, cycles_per_op(rdtsc() - t0, total_ops.load())});
        };

        default_slab ps{};
        run_hold("Slab (TLC)",   [&] { return ps.alloc(sz); }, [&](void* p) { ps.free(p, sz); });
        default_dynamic_slab ds{};
        run_hold("Dynamic Slab", [&] { return ds.palloc(sz); }, [&](void* p) { ds.free(p, sz); });
        run_hold("jemalloc",     [] { return mallocx(sz, 0); }, [](void* p) { dallocx(p, 0); });
        run_hold("malloc",       [] { return std::malloc(sz); }, [](void* p) { std::free(p); });

        print_results(results);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Test 8: Calloc (zero-initialized)
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 8: Calloc zero-initialized alloc+free (1M ops) ━━━\n\n";
        constexpr size_t ops = 1'000'000;
        constexpr size_t sizes[] = {32, 256, 1024, 4096};

        std::cout << "  " << std::left << std::setw(8) << "Size"
                  << std::right << std::setw(10) << "Slab" << std::setw(10) << "jemalloc" << std::setw(10) << "calloc"
                  << "  (cycles/op)\n";
        std::cout << "  " << std::string(38, '-') << "\n";

        for (size_t sz : sizes)
        {
            default_slab ps{};
            uint64_t t0 = rdtsc();
            for (size_t i = 0; i < ops; ++i) { void* p = ps.calloc(sz); escape(p); ps.free(p, sz); clobber(); }
            double slab = cycles_per_op(rdtsc() - t0, ops * 2);

            t0 = rdtsc();
            for (size_t i = 0; i < ops; ++i) { void* p = mallocx(sz, MALLOCX_ZERO); escape(p); dallocx(p, 0); clobber(); }
            double je = cycles_per_op(rdtsc() - t0, ops * 2);

            t0 = rdtsc();
            for (size_t i = 0; i < ops; ++i) { void* p = std::calloc(1, sz); escape(p); std::free(p); clobber(); }
            double mal = cycles_per_op(rdtsc() - t0, ops * 2);

            char line[128];
            std::snprintf(line, sizeof(line), "  %4zuB   %7.1f   %7.1f   %7.1f", sz, slab, je, mal);
            std::cout << line << "\n";
        }
        std::cout << "\n";
    }

    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    Showdown complete.                    ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
    return 0;
}
