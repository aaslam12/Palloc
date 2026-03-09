#include "arena.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <vector>

using namespace AL;

static const size_t PAGE_SIZE = getpagesize();

int main()
{
    const int SMALL_ALLOCS = 200000; // 100K allocations
    const int RESET_CYCLES = 100000;  // 10K cycles
    const int ALLOCS_PER_RESET = 1000; // 100 per reset
    const int ALLOC_SIZE = 100;

    std::cout << "\n========================================" << '\n';
    std::cout << "Arena vs Malloc Performance Comparison" << '\n';
    std::cout << "========================================\n" << '\n';
    std::cout << "Page size: " << PAGE_SIZE << " bytes\n" << '\n';

    // ========================================================================
    // Test 1: Many small allocations (no free)
    // ========================================================================
    {
        std::cout << "--- Test 1: Sequential Small Allocations (no free) ---" << '\n';
        std::cout << "Operations: " << SMALL_ALLOCS << " x 8 byte allocations" << '\n';

        // Test with Arena
        {
            std::cout << "\n[Testing Arena]" << '\n';
            AL::arena a(PAGE_SIZE * 1000);
            std::vector<void*> ptrs;

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < SMALL_ALLOCS; ++i)
            {
                void* ptr = a.alloc(8);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: Arena allocation failed at iteration " << i << '\n';
                    return 1;
                }
                ptrs.push_back(ptr);

                if ((i + 1) % 25000 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << SMALL_ALLOCS << '\n';
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;

            std::cout << "Arena time:       " << elapsed.count() << " s" << '\n';
            std::cout << "Avg per alloc:    " << (elapsed.count() * 1e6 / SMALL_ALLOCS) << " us" << '\n';
            std::cout << "Allocs per sec:   " << (SMALL_ALLOCS / elapsed.count()) << '\n';
        }

        // Test with malloc
        {
            std::cout << "\n[Testing malloc]" << '\n';
            std::vector<void*> ptrs;

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < SMALL_ALLOCS; ++i)
            {
                void* ptr = malloc(8);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: malloc failed at iteration " << i << '\n';
                    return 1;
                }
                ptrs.push_back(ptr);

                if ((i + 1) % 25000 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << SMALL_ALLOCS << '\n';
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;

            std::cout << "malloc time:      " << elapsed.count() << " s" << '\n';
            std::cout << "Avg per alloc:    " << (elapsed.count() * 1e6 / SMALL_ALLOCS) << " us" << '\n';
            std::cout << "Allocs per sec:   " << (SMALL_ALLOCS / elapsed.count()) << '\n';

            // Free all malloc'd memory
            for (void* ptr : ptrs)
            {
                free(ptr);
            }
        }

        std::cout << "\n[PASSED] Test 1 completed\n" << '\n';
    }

    // ========================================================================
    // Test 2: Repeated alloc/reset cycles
    // ========================================================================
    {
        std::cout << "--- Test 2: Repeated Alloc/Reset Cycles ---" << '\n';
        std::cout << "Cycles:           " << RESET_CYCLES << '\n';
        std::cout << "Allocs per cycle: " << ALLOCS_PER_RESET << " x " << ALLOC_SIZE << " bytes" << '\n';

        // Test with Arena
        {
            std::cout << "\n[Testing Arena with reset]" << '\n';
            AL::arena a(PAGE_SIZE * 32);

            auto start = std::chrono::high_resolution_clock::now();

            for (int cycle = 0; cycle < RESET_CYCLES; ++cycle)
            {
                for (int i = 0; i < ALLOCS_PER_RESET; ++i)
                {
                    void* ptr = a.alloc(ALLOC_SIZE);
                    if (ptr == nullptr)
                    {
                        std::cerr << "ERROR: Arena allocation failed at cycle " << cycle << ", iteration " << i << '\n';
                        return 1;
                    }
                }

                a.reset();

                if ((cycle + 1) % 2500 == 0)
                {
                    std::cout << "  Progress: " << (cycle + 1) << "/" << RESET_CYCLES << '\n';
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = RESET_CYCLES * (ALLOCS_PER_RESET + 1); // +1 for reset

            std::cout << "Arena time:       " << elapsed.count() << " s" << '\n';
            std::cout << "Total ops:        " << total_ops << " (allocs + resets)" << '\n';
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << '\n';
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << '\n';
        }

        // Test with malloc/free
        {
            std::cout << "\n[Testing malloc/free]" << '\n';

            auto start = std::chrono::high_resolution_clock::now();

            for (int cycle = 0; cycle < RESET_CYCLES; ++cycle)
            {
                std::vector<void*> ptrs;

                for (int i = 0; i < ALLOCS_PER_RESET; ++i)
                {
                    void* ptr = malloc(ALLOC_SIZE);
                    if (ptr == nullptr)
                    {
                        std::cerr << "ERROR: malloc failed at cycle " << cycle << ", iteration " << i << '\n';
                        return 1;
                    }
                    ptrs.push_back(ptr);
                }

                // Free all (equivalent to reset)
                for (void* ptr : ptrs)
                {
                    free(ptr);
                }

                if ((cycle + 1) % 2500 == 0)
                {
                    std::cout << "  Progress: " << (cycle + 1) << "/" << RESET_CYCLES << '\n';
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = RESET_CYCLES * (ALLOCS_PER_RESET * 2); // alloc + free

            std::cout << "malloc time:      " << elapsed.count() << " s" << '\n';
            std::cout << "Total ops:        " << total_ops << " (allocs + frees)" << '\n';
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << '\n';
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << '\n';
        }

        std::cout << "\n[PASSED] Test 2 completed\n" << '\n';
    }

    // ========================================================================
    // Test 3: Mixed size allocations
    // ========================================================================
    {
        std::cout << "--- Test 3: Mixed Size Allocations ---" << '\n';
        const int MIXED_ALLOCS = 50000;
        std::cout << "Operations: " << MIXED_ALLOCS << " allocations (sizes: 8, 16, 32, 64 bytes)" << '\n';

        // Test with Arena
        {
            std::cout << "\n[Testing Arena]" << '\n';
            AL::arena a(PAGE_SIZE * 500);
            std::vector<void*> ptrs;

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < MIXED_ALLOCS; ++i)
            {
                size_t size = 8 << (i % 4); // 8, 16, 32, 64
                void* ptr = a.alloc(size);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: Arena allocation failed at iteration " << i << '\n';
                    return 1;
                }
                ptrs.push_back(ptr);

                if ((i + 1) % 12500 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << MIXED_ALLOCS << '\n';
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;

            std::cout << "Arena time:       " << elapsed.count() << " s" << '\n';
            std::cout << "Avg per alloc:    " << (elapsed.count() * 1e6 / MIXED_ALLOCS) << " us" << '\n';
            std::cout << "Allocs per sec:   " << (MIXED_ALLOCS / elapsed.count()) << '\n';
        }

        // Test with malloc
        {
            std::cout << "\n[Testing malloc]" << '\n';
            std::vector<void*> ptrs;

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < MIXED_ALLOCS; ++i)
            {
                size_t size = 8 << (i % 4); // 8, 16, 32, 64
                void* ptr = malloc(size);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: malloc failed at iteration " << i << '\n';
                    return 1;
                }
                ptrs.push_back(ptr);

                if ((i + 1) % 12500 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << MIXED_ALLOCS << '\n';
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;

            std::cout << "malloc time:      " << elapsed.count() << " s" << '\n';
            std::cout << "Avg per alloc:    " << (elapsed.count() * 1e6 / MIXED_ALLOCS) << " us" << '\n';
            std::cout << "Allocs per sec:   " << (MIXED_ALLOCS / elapsed.count()) << '\n';

            // Free all malloc'd memory
            for (void* ptr : ptrs)
            {
                free(ptr);
            }
        }

        std::cout << "\n[PASSED] Test 3 completed\n" << '\n';
    }

    std::cout << "========================================" << '\n';
    std::cout << "[PASSED] All arena vs malloc tests passed!" << '\n';
    std::cout << "========================================" << '\n';

    return 0;
}
