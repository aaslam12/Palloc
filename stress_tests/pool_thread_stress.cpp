#include "pool.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <thread>
#include <unordered_set>
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
} // namespace

int main()
{
    const size_t threads = worker_count();

    std::cout << "\n=== Pool Threaded Stress Test ===" << '\n';
    std::cout << "Threads: " << threads << '\n' << '\n';

    // ========================================================================
    // Test 1: High-contention alloc/free churn
    // ========================================================================
    {
        const size_t block_size = 128;
        const size_t block_count = threads * 65536;
        const size_t iterations_per_thread = 10000000;
        pool p(block_size, block_count);

        std::atomic<bool> start{false};
        std::atomic<size_t> successful_cycles{0};
        std::vector<std::thread> workers;
        workers.reserve(threads);

        auto begin = std::chrono::high_resolution_clock::now();

        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&, tid] {
                wait_for_start(start);
                for (size_t i = 0; i < iterations_per_thread; ++i)
                {
                    void* ptr = p.alloc();
                    if (ptr == nullptr)
                        continue;

                    std::memset(ptr, static_cast<int>((tid + i) & 0xFF), block_size);
                    p.free(ptr);
                    successful_cycles.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers)
            t.join();

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - begin;

        if (successful_cycles.load(std::memory_order_relaxed) == 0)
        {
            std::cerr << "ERROR: No successful alloc/free cycles completed" << '\n';
            return 1;
        }

        if (p.get_free_space() != p.get_block_size() * p.get_block_count())
        {
            std::cerr << "ERROR: Pool did not fully recover after churn" << '\n';
            return 1;
        }

        const size_t total_ops = successful_cycles.load(std::memory_order_relaxed) * 2;
        std::cout << "--- Test 1: High-contention churn ---\n"
                  << "Successful cycles: " << successful_cycles.load(std::memory_order_relaxed) << '\n'
                  << "Elapsed:           " << elapsed.count() << " s\n"
                  << "Ops/sec:           " << (total_ops / elapsed.count()) << '\n'
                  << "[PASSED]\n"
                  << '\n';
    }

    // ========================================================================
    // Test 2: Concurrent full exhaustion and concurrent free
    // ========================================================================
    {
        const size_t block_size = 64;
        const size_t block_count = threads * 262144;
        pool p(block_size, block_count);

        std::atomic<bool> start{false};
        std::atomic<size_t> successful_allocs{0};
        std::vector<std::vector<void*>> allocated(threads);
        std::vector<std::thread> workers;
        workers.reserve(threads);

        auto begin = std::chrono::high_resolution_clock::now();

        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&, tid] {
                auto& local = allocated[tid];
                local.reserve(block_count / threads + 64);
                wait_for_start(start);

                while (true)
                {
                    void* ptr = p.alloc();
                    if (ptr == nullptr)
                        break;
                    local.push_back(ptr);
                    successful_allocs.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers)
            t.join();

        if (successful_allocs.load(std::memory_order_relaxed) != block_count)
        {
            std::cerr << "ERROR: Exhaustion mismatch. Expected " << block_count << " allocations, got "
                      << successful_allocs.load(std::memory_order_relaxed) << '\n';
            return 1;
        }

        std::unordered_set<void*> unique_ptrs;
        unique_ptrs.reserve(block_count);
        for (const auto& local : allocated)
        {
            for (void* ptr : local)
            {
                if (!unique_ptrs.insert(ptr).second)
                {
                    std::cerr << "ERROR: Duplicate pointer detected during full exhaustion" << '\n';
                    return 1;
                }
            }
        }
        if (unique_ptrs.size() != block_count)
        {
            std::cerr << "ERROR: Unique pointer count mismatch during full exhaustion" << '\n';
            return 1;
        }

        start.store(false, std::memory_order_release);
        workers.clear();
        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&, tid] {
                wait_for_start(start);
                for (void* ptr : allocated[tid])
                    p.free(ptr);
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers)
            t.join();

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - begin;

        if (p.get_free_space() != p.get_block_size() * p.get_block_count())
        {
            std::cerr << "ERROR: Pool not fully free after concurrent free phase" << '\n';
            return 1;
        }

        std::cout << "--- Test 2: Full exhaustion + concurrent free ---\n"
                  << "Blocks exhausted:   " << block_count << '\n'
                  << "Elapsed:            " << elapsed.count() << " s\n"
                  << "[PASSED]\n"
                  << '\n';
    }

    // ========================================================================
    // Test 3: Concurrent allocation cycles with synchronized reset
    // ========================================================================
    {
        const size_t block_size = 96;
        const size_t block_count = threads * 131072;
        const size_t allocs_per_thread = 256;
        const size_t cycles = 150;
        pool p(block_size, block_count);

        auto begin = std::chrono::high_resolution_clock::now();

        for (size_t cycle = 0; cycle < cycles; ++cycle)
        {
            std::atomic<bool> start{false};
            std::atomic<size_t> null_allocations{0};
            std::vector<std::vector<void*>> allocated(threads);
            std::vector<std::thread> workers;
            workers.reserve(threads);

            for (size_t tid = 0; tid < threads; ++tid)
            {
                workers.emplace_back([&, tid] {
                    auto& local = allocated[tid];
                    local.reserve(allocs_per_thread);
                    wait_for_start(start);
                    for (size_t i = 0; i < allocs_per_thread; ++i)
                    {
                        void* ptr = p.alloc();
                        if (ptr == nullptr)
                        {
                            null_allocations.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                        local.push_back(ptr);
                    }
                });
            }

            start.store(true, std::memory_order_release);
            for (auto& t : workers)
                t.join();

            if (null_allocations.load(std::memory_order_relaxed) != 0)
            {
                std::cerr << "ERROR: Unexpected allocation failure in cycle " << cycle << '\n';
                return 1;
            }

            p.reset();
            if (p.get_free_space() != p.get_block_size() * p.get_block_count())
            {
                std::cerr << "ERROR: Reset failed to restore free space in cycle " << cycle << '\n';
                return 1;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - begin;

        std::cout << "--- Test 3: Concurrent cycles + synchronized reset ---\n"
                  << "Cycles:             " << cycles << '\n'
                  << "Elapsed:            " << elapsed.count() << " s\n"
                  << "[PASSED]\n"
                  << '\n';
    }

    std::cout << "========================================" << '\n';
    std::cout << "[PASSED] All pool threaded stress tests passed!" << '\n';
    std::cout << "========================================\n" << '\n';
    return 0;
}
