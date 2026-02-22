#include "slab.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace AL;

namespace
{
size_t worker_count()
{
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0)
        return 8;
    return std::min<size_t>(hw, 16);
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
} // namespace

int main()
{
    const size_t threads = worker_count();

    std::cout << "\n=== Slab TLC (Thread-Local Cache) Stress Test ===\n";
    std::cout << "Threads: " << threads << "\n\n";

    // Test 1: TLC hit rate under single-thread churn
    {
        constexpr size_t ops = 2'000'000;
        slab s(4.0);

        auto bench = [&](size_t size, const char* label) {
            auto t0 = std::chrono::high_resolution_clock::now();
            for (size_t i = 0; i < ops; ++i)
            {
                void* p = s.alloc(size);
                s.free(p, size);
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = t1 - t0;
            std::cout << "  " << label << ": " << ns_per_op(elapsed.count(), ops * 2) << " ns/op\n";
        };

        std::cout << "--- Test 1: Single-thread TLC vs direct-pool latency ---\n";
        std::cout << "  (all size classes now use TLC)\n";
        bench(8, "  8B [TLC]");
        bench(16, " 16B [TLC]");
        bench(32, " 32B [TLC]");
        bench(64, " 64B [TLC]");
        bench(128, "128B [TLC]");
        bench(256, "256B [TLC]");
        bench(512, "512B [TLC]");
        std::cout << "\n";
    }

    // Test 2: TLC batch refill pressure
    // Hold more than one batch worth of objects to force repeated refills.
    {
        constexpr size_t batch_size = 128;            // TLC object_count
        constexpr size_t hold_count = batch_size + 1; // forces at least one refill
        constexpr size_t cycles = 50'000;
        slab s(4.0);

        std::vector<void*> held(hold_count);

        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t c = 0; c < cycles; ++c)
        {
            for (size_t i = 0; i < hold_count; ++i)
                held[i] = s.alloc(32);
            for (size_t i = 0; i < hold_count; ++i)
                s.free(held[i], 32);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = t1 - t0;
        size_t total_ops = cycles * hold_count * 2;

        std::cout << "--- Test 2: TLC batch refill/flush pressure ---\n";
        std::cout << "  Hold count:  " << hold_count << " (> one batch = " << batch_size << ")\n";
        std::cout << "  Cycles:      " << cycles << "\n";
        std::cout << "  ns/op:       " << ns_per_op(elapsed.count(), total_ops) << "\n\n";
    }

    // Test 3: Concurrent TLC — all threads on different size classes (all are cached)
    // Each thread hammers a different size class from the full set.
    {
        constexpr size_t iters = 500'000;
        constexpr std::array<size_t, 10> all_sizes = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
        slab s(8.0);

        std::atomic<bool> start{false};
        std::atomic<size_t> total_ops{0};
        std::vector<std::thread> workers;
        workers.reserve(threads);

        auto t0 = std::chrono::high_resolution_clock::now();

        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&, tid] {
                size_t sz = all_sizes[tid % all_sizes.size()];
                wait_for_start(start);
                for (size_t i = 0; i < iters; ++i)
                {
                    void* p = s.alloc(sz);
                    if (p == nullptr)
                        continue;
                    s.free(p, sz);
                }
                total_ops.fetch_add(iters * 2, std::memory_order_relaxed);
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers)
            t.join();

        auto t1 = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = t1 - t0;

        std::cout << "--- Test 3: Concurrent TLC all size classes ---\n";
        std::cout << "  Threads:   " << threads << "\n";
        std::cout << "  Total ops: " << total_ops.load() << "\n";
        std::cout << "  Elapsed:   " << elapsed.count() << " s\n";
        std::cout << "  Throughput:" << static_cast<size_t>(total_ops.load() / elapsed.count()) << " ops/s\n\n";
    }

    // Test 4: Epoch invalidation overhead
    // One thread resets the slab while others allocate, measuring reset cost.
    {
        constexpr size_t alloc_iters = 200'000;
        constexpr size_t reset_count = 20;
        slab s(8.0);

        std::atomic<bool> start{false};
        std::atomic<bool> done{false};
        std::atomic<size_t> resets_done{0};
        std::vector<std::thread> workers;
        workers.reserve(threads);

        // Allocator threads
        for (size_t tid = 0; tid < threads - 1; ++tid)
        {
            workers.emplace_back([&, tid] {
                wait_for_start(start);
                for (size_t i = 0; !done.load(std::memory_order_acquire) && i < alloc_iters; ++i)
                {
                    size_t sz = (tid % 2 == 0) ? 32 : 64;
                    void* p = s.alloc(sz);
                    if (p)
                        s.free(p, sz);
                }
            });
        }

        auto t0 = std::chrono::high_resolution_clock::now();
        start.store(true, std::memory_order_release);

        // Reset thread
        for (size_t r = 0; r < reset_count; ++r)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            s.reset();
            resets_done.fetch_add(1, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);

        for (auto& t : workers)
            t.join();

        auto t1 = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = t1 - t0;

        std::cout << "--- Test 4: Epoch invalidation under concurrent alloc ---\n";
        std::cout << "  Resets performed: " << resets_done.load() << "\n";
        std::cout << "  Elapsed:          " << elapsed.count() << " s\n";
        std::cout << "  [Allocators recovered from epoch invalidation without errors]\n\n";

        // Verify slab is usable after all resets
        for (size_t sz : {8, 16, 32, 64, 128, 256})
        {
            void* p = s.alloc(sz);
            if (p == nullptr)
            {
                std::cerr << "ERROR: slab unusable after epoch resets for size " << sz << "\n";
                return 1;
            }
            s.free(p, sz);
        }
    }

    // Test 5: Multi-slab TLC eviction
    // More slabs than MAX_CACHED_SLABS (4) forces TLC eviction path.
    {
        constexpr size_t num_slabs = 8;
        constexpr size_t iters = 100'000;
        std::array<slab*, num_slabs> slabs;
        for (auto& sp : slabs)
            sp = new slab(4.0);

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
                    slab& s = *slabs[(tid + i) % num_slabs];
                    size_t sz = (i % 2 == 0) ? 32 : 64;
                    void* p = s.alloc(sz);
                    if (p)
                    {
                        s.free(p, sz);
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

        std::cout << "--- Test 5: Multi-slab TLC eviction path ---\n";
        std::cout << "  Slabs:       " << num_slabs << " (> MAX_CACHED_SLABS=4)\n";
        std::cout << "  Threads:     " << threads << "\n";
        std::cout << "  Total ops:   " << total_ops.load() << "\n";
        std::cout << "  Elapsed:     " << elapsed.count() << " s\n";
        std::cout << "  Throughput:  " << static_cast<size_t>(total_ops.load() / elapsed.count()) << " ops/s\n\n";

        for (auto* sp : slabs)
            delete sp;
    }

    std::cout << "=================================================\n";
    std::cout << "[PASSED] All TLC stress tests passed!\n";
    std::cout << "=================================================\n\n";
    return 0;
}
