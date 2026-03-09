#include "arena.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
    const size_t alloc_size = 32;

    std::cout << "\n=== Arena Threaded Stress Test ===" << '\n';
    std::cout << "Threads: " << threads << ", alloc size: " << alloc_size << " bytes\n" << '\n';

    // ========================================================================
    // Test 1: Fully concurrent bulk allocation
    // ========================================================================
    {
        const size_t allocs_per_thread = 10000000;
        const size_t total_allocs = threads * allocs_per_thread;
        arena a(total_allocs * alloc_size);

        std::atomic<bool> start{false};
        std::atomic<size_t> null_allocations{0};
        std::vector<std::vector<void*>> allocated(threads);
        std::vector<std::thread> workers;
        workers.reserve(threads);

        auto begin = std::chrono::high_resolution_clock::now();

        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&, tid] {
                auto& local = allocated[tid];
                local.reserve(allocs_per_thread);
                wait_for_start(start);

                for (size_t i = 0; i < allocs_per_thread; ++i)
                {
                    void* ptr = a.alloc(alloc_size);
                    if (ptr == nullptr)
                    {
                        null_allocations.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    std::memset(ptr, static_cast<int>((tid + i) & 0xFF), alloc_size);
                    local.push_back(ptr);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers)
            t.join();

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - begin;

        if (null_allocations.load(std::memory_order_relaxed) != 0)
        {
            std::cerr << "ERROR: Unexpected allocation failures in bulk concurrent test" << '\n';
            return 1;
        }

        std::unordered_set<void*> unique_ptrs;
        unique_ptrs.reserve(total_allocs);
        size_t seen = 0;
        for (const auto& local : allocated)
        {
            for (void* ptr : local)
            {
                if (!unique_ptrs.insert(ptr).second)
                {
                    std::cerr << "ERROR: Duplicate pointer detected in concurrent allocations" << '\n';
                    return 1;
                }
            }
            seen += local.size();
        }

        if (seen != total_allocs)
        {
            std::cerr << "ERROR: Allocation count mismatch. Expected " << total_allocs << ", got " << seen << '\n';
            return 1;
        }

        if (a.get_used() != total_allocs * alloc_size)
        {
            std::cerr << "ERROR: Used bytes mismatch. Expected " << (total_allocs * alloc_size) << ", got " << a.get_used() << '\n';
            return 1;
        }

        std::cout << "--- Test 1: Bulk concurrent allocation ---\n"
                  << "Total allocations: " << total_allocs << '\n'
                  << "Elapsed:           " << elapsed.count() << " s\n"
                  << "Allocs/sec:        " << (total_allocs / elapsed.count()) << '\n'
                  << "[PASSED]\n"
                  << '\n';
    }

    // ========================================================================
    // Test 2: Exhaustion under contention
    // ========================================================================
    {
        const size_t capacity_slots = threads * 1000000;
        const size_t attempts_per_thread = (capacity_slots / threads) + 2000;
        arena a(capacity_slots * alloc_size);

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
                local.reserve(attempts_per_thread);
                wait_for_start(start);

                for (size_t i = 0; i < attempts_per_thread; ++i)
                {
                    void* ptr = a.alloc(alloc_size);
                    if (ptr == nullptr)
                        continue;
                    local.push_back(ptr);
                    successful_allocs.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers)
            t.join();

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - begin;

        const size_t success_count = successful_allocs.load(std::memory_order_relaxed);
        if (success_count != capacity_slots)
        {
            std::cerr << "ERROR: Contended exhaustion mismatch. Expected " << capacity_slots << " successful allocations, got " << success_count
                      << '\n';
            return 1;
        }

        if (a.get_used() != capacity_slots * alloc_size)
        {
            std::cerr << "ERROR: Used bytes mismatch after contention exhaustion" << '\n';
            return 1;
        }

        std::unordered_set<void*> unique_ptrs;
        unique_ptrs.reserve(capacity_slots);
        for (const auto& local : allocated)
        {
            for (void* ptr : local)
            {
                if (!unique_ptrs.insert(ptr).second)
                {
                    std::cerr << "ERROR: Duplicate pointer detected during exhaustion test" << '\n';
                    return 1;
                }
            }
        }
        if (unique_ptrs.size() != capacity_slots)
        {
            std::cerr << "ERROR: Unique pointer count mismatch in exhaustion test" << '\n';
            return 1;
        }

        std::cout << "--- Test 2: Contended exhaustion ---\n"
                  << "Successful allocs: " << success_count << '\n'
                  << "Elapsed:           " << elapsed.count() << " s\n"
                  << "[PASSED]\n"
                  << '\n';
    }

    // ========================================================================
    // Test 3: Concurrent allocation cycles with synchronized reset
    // ========================================================================
    {
        const size_t cycles = 75;
        const size_t allocs_per_thread_per_cycle = 50000;
        const size_t cycle_bytes = threads * allocs_per_thread_per_cycle * alloc_size;
        arena a(cycle_bytes);

        auto begin = std::chrono::high_resolution_clock::now();

        for (size_t cycle = 0; cycle < cycles; ++cycle)
        {
            std::atomic<bool> start{false};
            std::atomic<size_t> null_allocations{0};
            std::vector<std::thread> workers;
            workers.reserve(threads);

            for (size_t tid = 0; tid < threads; ++tid)
            {
                workers.emplace_back([&, tid] {
                    wait_for_start(start);
                    for (size_t i = 0; i < allocs_per_thread_per_cycle; ++i)
                    {
                        void* ptr = a.alloc(alloc_size);
                        if (ptr == nullptr)
                        {
                            null_allocations.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                        std::memset(ptr, static_cast<int>((tid + i + cycle) & 0xFF), alloc_size);
                    }
                });
            }

            start.store(true, std::memory_order_release);
            for (auto& t : workers)
                t.join();

            if (null_allocations.load(std::memory_order_relaxed) != 0)
            {
                std::cerr << "ERROR: Allocation failure in cycle " << cycle << '\n';
                return 1;
            }

            if (a.get_used() != cycle_bytes)
            {
                std::cerr << "ERROR: Used byte mismatch in cycle " << cycle << '\n';
                return 1;
            }

            if (a.reset() != 0 || a.get_used() != 0)
            {
                std::cerr << "ERROR: Reset failed in cycle " << cycle << '\n';
                return 1;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - begin;

        std::cout << "--- Test 3: Concurrent cycles + synchronized reset ---\n"
                  << "Cycles:            " << cycles << '\n'
                  << "Elapsed:           " << elapsed.count() << " s\n"
                  << "[PASSED]\n"
                  << '\n';
    }

    std::cout << "========================================" << '\n';
    std::cout << "[PASSED] All arena threaded stress tests passed!" << '\n';
    std::cout << "========================================\n" << '\n';
    return 0;
}
