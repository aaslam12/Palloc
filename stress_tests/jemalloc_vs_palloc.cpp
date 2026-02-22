#include "slab.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include <jemalloc/jemalloc.h>

using namespace AL;

namespace
{
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

double throughput(double elapsed_s, size_t ops)
{
    return static_cast<double>(ops) / elapsed_s / 1e6; // MOps/s
}

void print_header(const std::string& title)
{
    std::cout << "\n--- " << title << " ---\n";
}

void print_row(const char* label, double ns, double mops)
{
    std::cout << "  " << label << ": " << static_cast<size_t>(ns) << " ns/op | " << static_cast<size_t>(mops) << " MOps/s\n";
}
} // namespace

int main()
{
    const size_t threads = worker_count();

    std::cout << "=== Palloc Slab vs jemalloc vs system malloc ===\n";
    std::cout << "Threads (for MT tests): " << threads << "\n";

    // Test 1: Single-threaded alloc/free throughput per size class
    {
        print_header("Test 1: Single-threaded alloc+free throughput by size");
        std::cout << "  [Size]    Palloc          jemalloc        malloc\n";
        std::cout << "  ------    ----------      ----------      ----------\n";

        constexpr size_t ops = 1'000'000;
        constexpr size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024};

        for (size_t sz : sizes)
        {
            // Palloc
            slab ps(8.0);
            auto t0 = std::chrono::high_resolution_clock::now();
            for (size_t i = 0; i < ops; ++i)
            {
                void* p = ps.alloc(sz);
                ps.free(p, sz);
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> palloc_elapsed = t1 - t0;
            double palloc_ns = ns_per_op(palloc_elapsed.count(), ops * 2);
            double palloc_mops = throughput(palloc_elapsed.count(), ops * 2);

            // jemalloc
            t0 = std::chrono::high_resolution_clock::now();
            for (size_t i = 0; i < ops; ++i)
            {
                void* p = mallocx(sz, 0);
                dallocx(p, 0);
            }
            t1 = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> je_elapsed = t1 - t0;
            double je_ns = ns_per_op(je_elapsed.count(), ops * 2);
            double je_mops = throughput(je_elapsed.count(), ops * 2);

            // system malloc
            t0 = std::chrono::high_resolution_clock::now();
            for (size_t i = 0; i < ops; ++i)
            {
                void* p = std::malloc(sz);
                std::free(p);
            }
            t1 = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> sys_elapsed = t1 - t0;
            double sys_ns = ns_per_op(sys_elapsed.count(), ops * 2);
            double sys_mops = throughput(sys_elapsed.count(), ops * 2);

            char line[256];
            std::snprintf(line,
                          sizeof(line),
                          "  %4zuB    %5.0f ns %5.0f M    %5.0f ns %5.0f M    %5.0f ns %5.0f M",
                          sz,
                          palloc_ns,
                          palloc_mops,
                          je_ns,
                          je_mops,
                          sys_ns,
                          sys_mops);
            std::cout << line << "\n";
        }
    }

    // Test 2: Batch allocation (allocate N, free all N)
    {
        print_header("Test 2: Batch alloc then batch free (256 objects, size=64)");
        constexpr size_t batch = 256;
        constexpr size_t cycles = 200'000;
        constexpr size_t sz = 64;

        std::vector<void*> ptrs(batch);

        // Palloc
        slab ps(8.0);
        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t c = 0; c < cycles; ++c)
        {
            for (size_t i = 0; i < batch; ++i)
                ptrs[i] = ps.alloc(sz);
            for (size_t i = 0; i < batch; ++i)
                ps.free(ptrs[i], sz);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> pd = t1 - t0;
        print_row("Palloc  ", ns_per_op(pd.count(), cycles * batch * 2), throughput(pd.count(), cycles * batch * 2));

        // jemalloc
        t0 = std::chrono::high_resolution_clock::now();
        for (size_t c = 0; c < cycles; ++c)
        {
            for (size_t i = 0; i < batch; ++i)
                ptrs[i] = mallocx(sz, 0);
            for (size_t i = 0; i < batch; ++i)
                dallocx(ptrs[i], 0);
        }
        t1 = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> jd = t1 - t0;
        print_row("jemalloc", ns_per_op(jd.count(), cycles * batch * 2), throughput(jd.count(), cycles * batch * 2));

        // malloc
        t0 = std::chrono::high_resolution_clock::now();
        for (size_t c = 0; c < cycles; ++c)
        {
            for (size_t i = 0; i < batch; ++i)
                ptrs[i] = std::malloc(sz);
            for (size_t i = 0; i < batch; ++i)
                std::free(ptrs[i]);
        }
        t1 = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> sd = t1 - t0;
        print_row("malloc  ", ns_per_op(sd.count(), cycles * batch * 2), throughput(sd.count(), cycles * batch * 2));
    }

    // Test 3: Multi-threaded throughput (concurrent alloc/free, size=32)
    {
        print_header("Test 3: Multi-threaded alloc+free (threads=" + std::to_string(threads) + ", size=32)");
        constexpr size_t iters = 500'000;
        constexpr size_t sz = 32;

        auto run_mt = [&](const char* label, auto alloc_fn, auto free_fn) {
            std::atomic<bool> start{false};
            std::atomic<size_t> total_ops{0};
            std::vector<std::thread> workers;
            workers.reserve(threads);

            auto t0 = std::chrono::high_resolution_clock::now();
            for (size_t tid = 0; tid < threads; ++tid)
            {
                workers.emplace_back([&] {
                    wait_for_start(start);
                    for (size_t i = 0; i < iters; ++i)
                    {
                        void* p = alloc_fn();
                        if (p)
                        {
                            free_fn(p);
                            total_ops.fetch_add(2, std::memory_order_relaxed);
                        }
                    }
                });
            }
            start.store(true, std::memory_order_release);
            for (auto& t : workers)
                t.join();
            auto t1 = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = t1 - t0;
            size_t ops = total_ops.load();
            print_row(label, ns_per_op(elapsed.count(), ops), throughput(elapsed.count(), ops));
        };

        slab ps(8.0);
        run_mt("Palloc  ", [&] { return ps.alloc(sz); }, [&](void* p) { ps.free(p, sz); });
        run_mt("jemalloc", [&] { return mallocx(sz, 0); }, [](void* p) { dallocx(p, 0); });
        run_mt("malloc  ", [&] { return std::malloc(sz); }, [](void* p) { std::free(p); });
    }

    // Test 4: Mixed sizes multi-threaded
    {
        print_header("Test 4: Multi-threaded mixed sizes (threads=" + std::to_string(threads) + ")");
        constexpr size_t iters = 300'000;
        constexpr size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024};

        auto run_mixed = [&](const char* label, auto alloc_fn, auto free_fn) {
            std::atomic<bool> start{false};
            std::atomic<size_t> total_ops{0};
            std::vector<std::thread> workers;
            workers.reserve(threads);

            auto t0 = std::chrono::high_resolution_clock::now();
            for (size_t tid = 0; tid < threads; ++tid)
            {
                workers.emplace_back([&, tid] {
                    wait_for_start(start);
                    for (size_t i = 0; i < iters; ++i)
                    {
                        size_t sz = sizes[(tid + i) % 8];
                        void* p = alloc_fn(sz);
                        if (p)
                        {
                            free_fn(p, sz);
                            total_ops.fetch_add(2, std::memory_order_relaxed);
                        }
                    }
                });
            }
            start.store(true, std::memory_order_release);
            for (auto& t : workers)
                t.join();
            auto t1 = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = t1 - t0;
            size_t ops = total_ops.load();
            print_row(label, ns_per_op(elapsed.count(), ops), throughput(elapsed.count(), ops));
        };

        slab ps(8.0);
        run_mixed("Palloc  ", [&](size_t sz) { return ps.alloc(sz); }, [&](void* p, size_t sz) { ps.free(p, sz); });
        run_mixed("jemalloc", [](size_t sz) { return mallocx(sz, 0); }, [](void* p, size_t) { dallocx(p, 0); });
        run_mixed("malloc  ", [](size_t sz) { return std::malloc(sz); }, [](void* p, size_t) { std::free(p); });
    }

    std::cout << "\n=================================================\n";
    return 0;
}
