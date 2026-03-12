// Comprehensive benchmark: Palloc (Arena, Pool, Slab, Dynamic Slab) vs jemalloc vs glibc malloc
// Uses volatile sink + asm clobber to prevent compiler from optimizing away allocations.

#include "arena.h"
#include "dynamic_slab.h"
#include "pool.h"
#include "slab.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#include <jemalloc/jemalloc.h>

using namespace AL;
using clk = std::chrono::high_resolution_clock;

namespace
{

// Prevent the compiler from optimizing away a pointer
inline void escape(void* p)
{
    asm volatile("" : : "g"(p) : "memory");
}

// Force the compiler to assume memory could have been modified
inline void clobber()
{
    asm volatile("" : : : "memory");
}

size_t worker_count()
{
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0)
        return 8;
    return std::min<size_t>(hw, 8);
}

void wait_for_start(const std::atomic<bool>& start)
{
    while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
}

double ns_per_op(double elapsed_s, size_t ops)
{
    return (elapsed_s * 1e9) / static_cast<double>(ops);
}

double mops_per_s(double elapsed_s, size_t ops)
{
    return static_cast<double>(ops) / elapsed_s / 1e6;
}

struct result
{
    const char* label;
    double ns;
    double mops;
};

void print_table_header()
{
    std::cout << "  " << std::left << std::setw(20) << "Allocator" << std::right << std::setw(10) << "ns/op"
              << std::setw(12) << "MOps/s" << "\n";
    std::cout << "  " << std::string(42, '-') << "\n";
}

void print_row(const result& r)
{
    std::cout << "  " << std::left << std::setw(20) << r.label << std::right << std::setw(8) << std::fixed
              << std::setprecision(1) << r.ns << std::setw(12) << std::setprecision(1) << r.mops << "\n";
}

void print_results(std::vector<result>& results)
{
    // Sort by ns ascending (fastest first)
    std::sort(results.begin(), results.end(), [](const result& a, const result& b) { return a.ns < b.ns; });
    print_table_header();
    for (const auto& r : results)
        print_row(r);
    std::cout << "\n";
}

} // namespace

int main()
{
    const size_t threads = worker_count();

    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║        Allocator Showdown: Palloc vs jemalloc vs malloc ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Threads: " << std::left << std::setw(47) << threads << "║\n";
    std::cout << "║  Compiler: GCC -O3                                      ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";

    // ─────────────────────────────────────────────────────────────────────────
    // Test 1: Single-threaded alloc+free throughput per size class
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 1: Single-threaded alloc+free (1M cycles per size) ━━━\n\n";
        constexpr size_t ops = 1'000'000;
        constexpr size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

        std::cout << "  " << std::left << std::setw(8) << "Size" << std::right << std::setw(12) << "Slab"
                  << std::setw(12) << "DynSlab" << std::setw(12) << "jemalloc" << std::setw(12) << "malloc"
                  << "  (ns/op)\n";
        std::cout << "  " << std::string(56, '-') << "\n";

        for (size_t sz : sizes)
        {
            // Palloc Slab
            default_slab ps{};
            auto t0 = clk::now();
            for (size_t i = 0; i < ops; ++i)
            {
                void* p = ps.alloc(sz);
                escape(p);
                ps.free(p, sz);
                clobber();
            }
            double slab_ns = ns_per_op(std::chrono::duration<double>(clk::now() - t0).count(), ops * 2);

            // Dynamic Slab
            default_dynamic_slab ds{};
            t0 = clk::now();
            for (size_t i = 0; i < ops; ++i)
            {
                void* p = ds.palloc(sz);
                escape(p);
                ds.free(p, sz);
                clobber();
            }
            double dslab_ns = ns_per_op(std::chrono::duration<double>(clk::now() - t0).count(), ops * 2);

            // jemalloc
            t0 = clk::now();
            for (size_t i = 0; i < ops; ++i)
            {
                void* p = mallocx(sz, 0);
                escape(p);
                dallocx(p, 0);
                clobber();
            }
            double je_ns = ns_per_op(std::chrono::duration<double>(clk::now() - t0).count(), ops * 2);

            // glibc malloc
            t0 = clk::now();
            for (size_t i = 0; i < ops; ++i)
            {
                void* p = std::malloc(sz);
                escape(p);
                std::free(p);
                clobber();
            }
            double mal_ns = ns_per_op(std::chrono::duration<double>(clk::now() - t0).count(), ops * 2);

            char line[128];
            std::snprintf(line, sizeof(line), "  %4zuB   %8.1f    %8.1f    %8.1f    %8.1f", sz, slab_ns, dslab_ns,
                          je_ns, mal_ns);
            std::cout << line << "\n";
        }
        std::cout << "\n";
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Test 2: Single-threaded linear arena bump vs malloc vs jemalloc
    // Arena is a linear allocator — only alloc, no individual free.
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 2: Single-threaded linear allocation (no free, 64B, 1M ops) ━━━\n\n";
        constexpr size_t ops = 1'000'000;
        constexpr size_t sz = 64;
        std::vector<result> results;

        // Arena
        arena a(ops * sz);
        auto t0 = clk::now();
        for (size_t i = 0; i < ops; ++i)
        {
            void* p = a.alloc(sz);
            escape(p);
        }
        double elapsed = std::chrono::duration<double>(clk::now() - t0).count();
        results.push_back({"Arena", ns_per_op(elapsed, ops), mops_per_s(elapsed, ops)});

        // Pool (fixed-size alloc only)
        pool po(sz, ops);
        t0 = clk::now();
        for (size_t i = 0; i < ops; ++i)
        {
            void* p = po.alloc();
            escape(p);
        }
        elapsed = std::chrono::duration<double>(clk::now() - t0).count();
        results.push_back({"Pool", ns_per_op(elapsed, ops), mops_per_s(elapsed, ops)});

        // jemalloc (alloc only, free at end)
        std::vector<void*> ptrs(ops);
        t0 = clk::now();
        for (size_t i = 0; i < ops; ++i)
        {
            ptrs[i] = mallocx(sz, 0);
            escape(ptrs[i]);
        }
        elapsed = std::chrono::duration<double>(clk::now() - t0).count();
        results.push_back({"jemalloc", ns_per_op(elapsed, ops), mops_per_s(elapsed, ops)});
        for (auto* p : ptrs)
            dallocx(p, 0);

        // malloc (alloc only, free at end)
        t0 = clk::now();
        for (size_t i = 0; i < ops; ++i)
        {
            ptrs[i] = std::malloc(sz);
            escape(ptrs[i]);
        }
        elapsed = std::chrono::duration<double>(clk::now() - t0).count();
        results.push_back({"malloc", ns_per_op(elapsed, ops), mops_per_s(elapsed, ops)});
        for (auto* p : ptrs)
            std::free(p);

        print_results(results);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Test 3: Fixed-size pool alloc+free vs malloc(fixed) vs jemalloc(fixed)
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 3: Single-threaded fixed-size alloc+free (64B, 1M cycles) ━━━\n\n";
        constexpr size_t ops = 1'000'000;
        constexpr size_t sz = 64;
        std::vector<result> results;

        // Pool
        pool po(sz, ops);
        auto t0 = clk::now();
        for (size_t i = 0; i < ops; ++i)
        {
            void* p = po.alloc();
            escape(p);
            po.free(p);
            clobber();
        }
        double elapsed = std::chrono::duration<double>(clk::now() - t0).count();
        results.push_back({"Pool", ns_per_op(elapsed, ops * 2), mops_per_s(elapsed, ops * 2)});

        // Slab
        default_slab s{};
        t0 = clk::now();
        for (size_t i = 0; i < ops; ++i)
        {
            void* p = s.alloc(sz);
            escape(p);
            s.free(p, sz);
            clobber();
        }
        elapsed = std::chrono::duration<double>(clk::now() - t0).count();
        results.push_back({"Slab (TLC)", ns_per_op(elapsed, ops * 2), mops_per_s(elapsed, ops * 2)});

        // jemalloc
        t0 = clk::now();
        for (size_t i = 0; i < ops; ++i)
        {
            void* p = mallocx(sz, 0);
            escape(p);
            dallocx(p, 0);
            clobber();
        }
        elapsed = std::chrono::duration<double>(clk::now() - t0).count();
        results.push_back({"jemalloc", ns_per_op(elapsed, ops * 2), mops_per_s(elapsed, ops * 2)});

        // malloc
        t0 = clk::now();
        for (size_t i = 0; i < ops; ++i)
        {
            void* p = std::malloc(sz);
            escape(p);
            std::free(p);
            clobber();
        }
        elapsed = std::chrono::duration<double>(clk::now() - t0).count();
        results.push_back({"malloc", ns_per_op(elapsed, ops * 2), mops_per_s(elapsed, ops * 2)});

        print_results(results);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Test 4: Batch alloc then batch free (realistic pattern — hold many objects)
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 4: Batch alloc-then-free (256 objects × 200K cycles, 64B) ━━━\n\n";
        constexpr size_t batch = 256;
        constexpr size_t cycles = 200'000;
        constexpr size_t sz = 64;
        std::vector<void*> ptrs(batch);
        std::vector<result> results;

        // Slab
        default_slab ps{};
        auto t0 = clk::now();
        for (size_t c = 0; c < cycles; ++c)
        {
            for (size_t i = 0; i < batch; ++i)
            {
                ptrs[i] = ps.alloc(sz);
                escape(ptrs[i]);
            }
            for (size_t i = 0; i < batch; ++i)
            {
                ps.free(ptrs[i], sz);
                clobber();
            }
        }
        double elapsed = std::chrono::duration<double>(clk::now() - t0).count();
        results.push_back({"Slab (TLC)", ns_per_op(elapsed, cycles * batch * 2), mops_per_s(elapsed, cycles * batch * 2)});

        // Dynamic Slab
        default_dynamic_slab ds{};
        t0 = clk::now();
        for (size_t c = 0; c < cycles; ++c)
        {
            for (size_t i = 0; i < batch; ++i)
            {
                ptrs[i] = ds.palloc(sz);
                escape(ptrs[i]);
            }
            for (size_t i = 0; i < batch; ++i)
            {
                ds.free(ptrs[i], sz);
                clobber();
            }
        }
        elapsed = std::chrono::duration<double>(clk::now() - t0).count();
        results.push_back({"Dynamic Slab", ns_per_op(elapsed, cycles * batch * 2), mops_per_s(elapsed, cycles * batch * 2)});

        // jemalloc
        t0 = clk::now();
        for (size_t c = 0; c < cycles; ++c)
        {
            for (size_t i = 0; i < batch; ++i)
            {
                ptrs[i] = mallocx(sz, 0);
                escape(ptrs[i]);
            }
            for (size_t i = 0; i < batch; ++i)
            {
                dallocx(ptrs[i], 0);
                clobber();
            }
        }
        elapsed = std::chrono::duration<double>(clk::now() - t0).count();
        results.push_back({"jemalloc", ns_per_op(elapsed, cycles * batch * 2), mops_per_s(elapsed, cycles * batch * 2)});

        // malloc
        t0 = clk::now();
        for (size_t c = 0; c < cycles; ++c)
        {
            for (size_t i = 0; i < batch; ++i)
            {
                ptrs[i] = std::malloc(sz);
                escape(ptrs[i]);
            }
            for (size_t i = 0; i < batch; ++i)
            {
                std::free(ptrs[i]);
                clobber();
            }
        }
        elapsed = std::chrono::duration<double>(clk::now() - t0).count();
        results.push_back({"malloc", ns_per_op(elapsed, cycles * batch * 2), mops_per_s(elapsed, cycles * batch * 2)});

        print_results(results);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Test 5: Multi-threaded alloc+free — single size class contention
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 5: Multi-threaded alloc+free (" << threads << " threads, 32B, 500K iters) ━━━\n\n";
        constexpr size_t iters = 500'000;
        constexpr size_t sz = 32;
        std::vector<result> results;

        auto run_mt = [&](const char* label, auto alloc_fn, auto free_fn) -> result {
            std::atomic<bool> start{false};
            std::atomic<size_t> total_ops{0};
            std::vector<std::thread> workers;
            workers.reserve(threads);

            auto t0 = clk::now();
            for (size_t tid = 0; tid < threads; ++tid)
            {
                workers.emplace_back([&] {
                    wait_for_start(start);
                    for (size_t i = 0; i < iters; ++i)
                    {
                        void* p = alloc_fn();
                        escape(p);
                        if (p)
                        {
                            free_fn(p);
                            clobber();
                            total_ops.fetch_add(2, std::memory_order_relaxed);
                        }
                    }
                });
            }
            start.store(true, std::memory_order_release);
            for (auto& t : workers)
                t.join();
            double elapsed = std::chrono::duration<double>(clk::now() - t0).count();
            size_t ops = total_ops.load();
            return {label, ns_per_op(elapsed, ops), mops_per_s(elapsed, ops)};
        };

        default_slab ps{};
        results.push_back(run_mt("Slab (TLC)", [&] { return ps.alloc(sz); }, [&](void* p) { ps.free(p, sz); }));

        default_dynamic_slab ds{};
        results.push_back(
            run_mt("Dynamic Slab", [&] { return ds.palloc(sz); }, [&](void* p) { ds.free(p, sz); }));

        results.push_back(run_mt("jemalloc", [] { return mallocx(sz, 0); }, [](void* p) { dallocx(p, 0); }));
        results.push_back(run_mt("malloc", [] { return std::malloc(sz); }, [](void* p) { std::free(p); }));

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

        auto run_mixed = [&](const char* label, auto alloc_fn, auto free_fn) -> result {
            std::atomic<bool> start{false};
            std::atomic<size_t> total_ops{0};
            std::vector<std::thread> workers;
            workers.reserve(threads);

            auto t0 = clk::now();
            for (size_t tid = 0; tid < threads; ++tid)
            {
                workers.emplace_back([&, tid] {
                    wait_for_start(start);
                    for (size_t i = 0; i < iters; ++i)
                    {
                        size_t sz = sizes[(tid + i) % 10];
                        void* p = alloc_fn(sz);
                        escape(p);
                        if (p)
                        {
                            free_fn(p, sz);
                            clobber();
                            total_ops.fetch_add(2, std::memory_order_relaxed);
                        }
                    }
                });
            }
            start.store(true, std::memory_order_release);
            for (auto& t : workers)
                t.join();
            double elapsed = std::chrono::duration<double>(clk::now() - t0).count();
            size_t ops = total_ops.load();
            return {label, ns_per_op(elapsed, ops), mops_per_s(elapsed, ops)};
        };

        default_slab ps{};
        results.push_back(
            run_mixed("Slab (TLC)", [&](size_t sz) { return ps.alloc(sz); }, [&](void* p, size_t sz) { ps.free(p, sz); }));

        default_dynamic_slab ds{};
        results.push_back(
            run_mixed("Dynamic Slab", [&](size_t sz) { return ds.palloc(sz); }, [&](void* p, size_t sz) { ds.free(p, sz); }));

        results.push_back(
            run_mixed("jemalloc", [](size_t sz) { return mallocx(sz, 0); }, [](void* p, size_t) { dallocx(p, 0); }));

        results.push_back(
            run_mixed("malloc", [](size_t sz) { return std::malloc(sz); }, [](void* p, size_t) { std::free(p); }));

        print_results(results);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Test 7: Multi-threaded batch alloc-then-free (long-lived objects)
    // This is the hardest pattern for Palloc's dynamic_slab.
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 7: MT batch hold pattern (" << threads << " threads, hold 500, 100 cycles) ━━━\n\n";
        constexpr size_t hold = 500;
        constexpr size_t cycles = 100;
        constexpr size_t sz = 64;
        std::vector<result> results;

        auto run_hold = [&](const char* label, auto alloc_fn, auto free_fn) -> result {
            std::atomic<bool> start{false};
            std::atomic<size_t> total_ops{0};
            std::vector<std::thread> workers;
            workers.reserve(threads);

            auto t0 = clk::now();
            for (size_t tid = 0; tid < threads; ++tid)
            {
                workers.emplace_back([&] {
                    std::vector<void*> ptrs(hold);
                    wait_for_start(start);
                    for (size_t c = 0; c < cycles; ++c)
                    {
                        for (size_t i = 0; i < hold; ++i)
                        {
                            ptrs[i] = alloc_fn();
                            escape(ptrs[i]);
                        }
                        for (size_t i = 0; i < hold; ++i)
                        {
                            if (ptrs[i])
                            {
                                free_fn(ptrs[i]);
                                clobber();
                            }
                        }
                        total_ops.fetch_add(hold * 2, std::memory_order_relaxed);
                    }
                });
            }
            start.store(true, std::memory_order_release);
            for (auto& t : workers)
                t.join();
            double elapsed = std::chrono::duration<double>(clk::now() - t0).count();
            size_t ops = total_ops.load();
            return {label, ns_per_op(elapsed, ops), mops_per_s(elapsed, ops)};
        };

        default_slab ps{};
        results.push_back(run_hold("Slab (TLC)", [&] { return ps.alloc(sz); }, [&](void* p) { ps.free(p, sz); }));

        default_dynamic_slab ds{};
        results.push_back(
            run_hold("Dynamic Slab", [&] { return ds.palloc(sz); }, [&](void* p) { ds.free(p, sz); }));

        results.push_back(run_hold("jemalloc", [] { return mallocx(sz, 0); }, [](void* p) { dallocx(p, 0); }));
        results.push_back(run_hold("malloc", [] { return std::malloc(sz); }, [](void* p) { std::free(p); }));

        print_results(results);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Test 8: Calloc comparison (zero-initialized allocation)
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "━━━ Test 8: Single-threaded calloc (zeroed alloc+free, 1M cycles) ━━━\n\n";
        constexpr size_t ops = 1'000'000;
        constexpr size_t sizes[] = {32, 256, 1024, 4096};
        std::vector<result> results;

        std::cout << "  " << std::left << std::setw(8) << "Size" << std::right << std::setw(12) << "Slab"
                  << std::setw(12) << "jemalloc" << std::setw(12) << "calloc"
                  << "  (ns/op)\n";
        std::cout << "  " << std::string(44, '-') << "\n";

        for (size_t sz : sizes)
        {
            // Slab calloc
            default_slab ps{};
            auto t0 = clk::now();
            for (size_t i = 0; i < ops; ++i)
            {
                void* p = ps.calloc(sz);
                escape(p);
                ps.free(p, sz);
                clobber();
            }
            double slab_ns = ns_per_op(std::chrono::duration<double>(clk::now() - t0).count(), ops * 2);

            // jemalloc calloc (mallocx + MALLOCX_ZERO)
            t0 = clk::now();
            for (size_t i = 0; i < ops; ++i)
            {
                void* p = mallocx(sz, MALLOCX_ZERO);
                escape(p);
                dallocx(p, 0);
                clobber();
            }
            double je_ns = ns_per_op(std::chrono::duration<double>(clk::now() - t0).count(), ops * 2);

            // glibc calloc
            t0 = clk::now();
            for (size_t i = 0; i < ops; ++i)
            {
                void* p = std::calloc(1, sz);
                escape(p);
                std::free(p);
                clobber();
            }
            double mal_ns = ns_per_op(std::chrono::duration<double>(clk::now() - t0).count(), ops * 2);

            char line[128];
            std::snprintf(line, sizeof(line), "  %4zuB   %8.1f    %8.1f    %8.1f", sz, slab_ns, je_ns, mal_ns);
            std::cout << line << "\n";
        }
        std::cout << "\n";
    }

    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    Showdown complete.                    ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
    return 0;
}
