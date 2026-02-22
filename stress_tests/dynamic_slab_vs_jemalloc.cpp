#include "dynamic_slab.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
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
} // namespace

int main()
{
    const size_t threads = worker_count();

    std::cout << "=== Dynamic Slab vs jemalloc (unbounded allocation) ===\n";
    std::cout << "Threads: " << threads << "\n\n";

    // Test 1: Single-threaded throughput with long-lived allocations
    {
        std::cout << "--- Test 1: Single-threaded long-lived alloc (hold 1000, then free) ---\n";
        constexpr size_t hold = 1000;
        constexpr size_t cycles = 1000;
        constexpr size_t sz = 64;

        std::vector<void*> ptrs(hold);

        // Dynamic Slab
        dynamic_slab ds(1.0);
        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t c = 0; c < cycles; ++c)
        {
            for (size_t i = 0; i < hold; ++i)
                ptrs[i] = ds.palloc(sz);
            for (size_t i = 0; i < hold; ++i)
                ds.free(ptrs[i], sz);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> ds_time = t1 - t0;
        std::cout << "  Dynamic Slab: " << ns_per_op(ds_time.count(), cycles * hold * 2) << " ns/op | "
                  << throughput(ds_time.count(), cycles * hold * 2) << " MOps/s\n";
        std::cout << "  Slabs created: " << ds.get_slab_count() << "\n";

        // jemalloc
        t0 = std::chrono::high_resolution_clock::now();
        for (size_t c = 0; c < cycles; ++c)
        {
            for (size_t i = 0; i < hold; ++i)
                ptrs[i] = mallocx(sz, 0);
            for (size_t i = 0; i < hold; ++i)
                dallocx(ptrs[i], 0);
        }
        t1 = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> je_time = t1 - t0;
        std::cout << "  jemalloc:      " << ns_per_op(je_time.count(), cycles * hold * 2) << " ns/op | "
                  << throughput(je_time.count(), cycles * hold * 2) << " MOps/s\n\n";
    }

    // Test 2: Multi-threaded with many long-lived allocations
    {
        std::cout << "--- Test 2: Multi-threaded long-lived (threads=" << threads << ", hold 500 each) ---\n";
        constexpr size_t iters = 100;
        constexpr size_t sz = 32;

        auto run_mt = [&](const char* label, auto alloc_fn, auto free_fn) {
            std::atomic<bool> start{false};
            std::atomic<size_t> total_ops{0};
            std::vector<std::thread> workers;
            workers.reserve(threads);

            auto t0 = std::chrono::high_resolution_clock::now();
            for (size_t tid = 0; tid < threads; ++tid)
            {
                workers.emplace_back([&, tid] {
                    std::vector<void*> ptrs(500);
                    wait_for_start(start);
                    for (size_t i = 0; i < iters; ++i)
                    {
                        for (size_t j = 0; j < 500; ++j)
                            ptrs[j] = alloc_fn();
                        for (size_t j = 0; j < 500; ++j)
                            free_fn(ptrs[j]);
                        total_ops.fetch_add(1000, std::memory_order_relaxed);
                    }
                });
            }
            start.store(true, std::memory_order_release);
            for (auto& t : workers)
                t.join();
            auto t1 = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = t1 - t0;
            size_t ops = total_ops.load();
            std::cout << "  " << label << ": " << ns_per_op(elapsed.count(), ops) << " ns/op | " << throughput(elapsed.count(), ops) << " MOps/s\n";
        };

        dynamic_slab ds(1.0);
        run_mt("Dynamic Slab", [&] { return ds.palloc(sz); }, [&](void* p) { ds.free(p, sz); });
        run_mt("jemalloc    ", [&] { return mallocx(sz, 0); }, [](void* p) { dallocx(p, 0); });
        std::cout << "\n";
    }

    // Test 3: Mixed sizes with concurrent allocation
    {
        std::cout << "--- Test 3: Multi-threaded mixed sizes (threads=" << threads << ") ---\n";
        constexpr size_t iters = 200;
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
                    std::vector<void*> ptrs(100);
                    wait_for_start(start);
                    for (size_t i = 0; i < iters; ++i)
                    {
                        for (size_t j = 0; j < 100; ++j)
                        {
                            size_t sz = sizes[(tid + i + j) % 8];
                            ptrs[j] = alloc_fn(sz);
                        }
                        for (size_t j = 0; j < 100; ++j)
                        {
                            size_t sz = sizes[(tid + i + j) % 8];
                            free_fn(ptrs[j], sz);
                        }
                        total_ops.fetch_add(200, std::memory_order_relaxed);
                    }
                });
            }
            start.store(true, std::memory_order_release);
            for (auto& t : workers)
                t.join();
            auto t1 = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = t1 - t0;
            size_t ops = total_ops.load();
            std::cout << "  " << label << ": " << ns_per_op(elapsed.count(), ops) << " ns/op | " << throughput(elapsed.count(), ops) << " MOps/s\n";
        };

        dynamic_slab ds(2.0);
        run_mixed("Dynamic Slab", [&](size_t sz) { return ds.palloc(sz); }, [&](void* p, size_t sz) { ds.free(p, sz); });
        run_mixed("jemalloc    ", [](size_t sz) { return mallocx(sz, 0); }, [](void* p, size_t) { dallocx(p, 0); });
        std::cout << "\n";
    }

    std::cout << "=== Unbounded allocation comparison complete ===\n";
    return 0;
}
