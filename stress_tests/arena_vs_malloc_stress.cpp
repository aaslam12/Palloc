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
    const int SMALL_ALLOCS = 100000;       // 100K allocations
    const int RESET_CYCLES = 10000;        // 10K cycles
    const int ALLOCS_PER_RESET = 100;      // 100 per reset
    const int ALLOC_SIZE = 100;

    std::cout << "\n========================================" << std::endl;
    std::cout << "Arena vs Malloc Performance Comparison" << std::endl;
    std::cout << "========================================\n" << std::endl;
    std::cout << "Page size: " << PAGE_SIZE << " bytes\n" << std::endl;

    // ========================================================================
    // Test 1: Many small allocations (no free)
    // ========================================================================
    {
        std::cout << "--- Test 1: Sequential Small Allocations (no free) ---" << std::endl;
        std::cout << "Operations: " << SMALL_ALLOCS << " x 8 byte allocations" << std::endl;

        // Test with Arena
        {
            std::cout << "\n[Testing Arena]" << std::endl;
            AL::arena a(PAGE_SIZE * 1000);
            std::vector<void*> ptrs;

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < SMALL_ALLOCS; ++i)
            {
                void* ptr = a.alloc(8);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: Arena allocation failed at iteration " << i << std::endl;
                    return 1;
                }
                ptrs.push_back(ptr);

                if ((i + 1) % 25000 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << SMALL_ALLOCS << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;

            std::cout << "Arena time:       " << elapsed.count() << " s" << std::endl;
            std::cout << "Avg per alloc:    " << (elapsed.count() * 1e6 / SMALL_ALLOCS) << " us" << std::endl;
            std::cout << "Allocs per sec:   " << (SMALL_ALLOCS / elapsed.count()) << std::endl;
        }

        // Test with malloc
        {
            std::cout << "\n[Testing malloc]" << std::endl;
            std::vector<void*> ptrs;

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < SMALL_ALLOCS; ++i)
            {
                void* ptr = malloc(8);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: malloc failed at iteration " << i << std::endl;
                    return 1;
                }
                ptrs.push_back(ptr);

                if ((i + 1) % 25000 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << SMALL_ALLOCS << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;

            std::cout << "malloc time:      " << elapsed.count() << " s" << std::endl;
            std::cout << "Avg per alloc:    " << (elapsed.count() * 1e6 / SMALL_ALLOCS) << " us" << std::endl;
            std::cout << "Allocs per sec:   " << (SMALL_ALLOCS / elapsed.count()) << std::endl;

            // Free all malloc'd memory
            for (void* ptr : ptrs)
            {
                free(ptr);
            }
        }

        std::cout << "\n[PASSED] Test 1 completed\n" << std::endl;
    }

    // ========================================================================
    // Test 2: Repeated alloc/reset cycles
    // ========================================================================
    {
        std::cout << "--- Test 2: Repeated Alloc/Reset Cycles ---" << std::endl;
        std::cout << "Cycles:           " << RESET_CYCLES << std::endl;
        std::cout << "Allocs per cycle: " << ALLOCS_PER_RESET << " x " << ALLOC_SIZE << " bytes" << std::endl;

        // Test with Arena
        {
            std::cout << "\n[Testing Arena with reset]" << std::endl;
            AL::arena a(PAGE_SIZE * 4);

            auto start = std::chrono::high_resolution_clock::now();

            for (int cycle = 0; cycle < RESET_CYCLES; ++cycle)
            {
                for (int i = 0; i < ALLOCS_PER_RESET; ++i)
                {
                    void* ptr = a.alloc(ALLOC_SIZE);
                    if (ptr == nullptr)
                    {
                        std::cerr << "ERROR: Arena allocation failed at cycle " << cycle
                                  << ", iteration " << i << std::endl;
                        return 1;
                    }
                }

                a.reset();

                if ((cycle + 1) % 2500 == 0)
                {
                    std::cout << "  Progress: " << (cycle + 1) << "/" << RESET_CYCLES << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = RESET_CYCLES * (ALLOCS_PER_RESET + 1); // +1 for reset

            std::cout << "Arena time:       " << elapsed.count() << " s" << std::endl;
            std::cout << "Total ops:        " << total_ops << " (allocs + resets)" << std::endl;
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << std::endl;
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << std::endl;
        }

        // Test with malloc/free
        {
            std::cout << "\n[Testing malloc/free]" << std::endl;

            auto start = std::chrono::high_resolution_clock::now();

            for (int cycle = 0; cycle < RESET_CYCLES; ++cycle)
            {
                std::vector<void*> ptrs;

                for (int i = 0; i < ALLOCS_PER_RESET; ++i)
                {
                    void* ptr = malloc(ALLOC_SIZE);
                    if (ptr == nullptr)
                    {
                        std::cerr << "ERROR: malloc failed at cycle " << cycle
                                  << ", iteration " << i << std::endl;
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
                    std::cout << "  Progress: " << (cycle + 1) << "/" << RESET_CYCLES << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = RESET_CYCLES * (ALLOCS_PER_RESET * 2); // alloc + free

            std::cout << "malloc time:      " << elapsed.count() << " s" << std::endl;
            std::cout << "Total ops:        " << total_ops << " (allocs + frees)" << std::endl;
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << std::endl;
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << std::endl;
        }

        std::cout << "\n[PASSED] Test 2 completed\n" << std::endl;
    }

    // ========================================================================
    // Test 3: Mixed size allocations
    // ========================================================================
    {
        std::cout << "--- Test 3: Mixed Size Allocations ---" << std::endl;
        const int MIXED_ALLOCS = 50000;
        std::cout << "Operations: " << MIXED_ALLOCS << " allocations (sizes: 8, 16, 32, 64 bytes)" << std::endl;

        // Test with Arena
        {
            std::cout << "\n[Testing Arena]" << std::endl;
            AL::arena a(PAGE_SIZE * 500);
            std::vector<void*> ptrs;

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < MIXED_ALLOCS; ++i)
            {
                size_t size = 8 << (i % 4); // 8, 16, 32, 64
                void* ptr = a.alloc(size);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: Arena allocation failed at iteration " << i << std::endl;
                    return 1;
                }
                ptrs.push_back(ptr);

                if ((i + 1) % 12500 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << MIXED_ALLOCS << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;

            std::cout << "Arena time:       " << elapsed.count() << " s" << std::endl;
            std::cout << "Avg per alloc:    " << (elapsed.count() * 1e6 / MIXED_ALLOCS) << " us" << std::endl;
            std::cout << "Allocs per sec:   " << (MIXED_ALLOCS / elapsed.count()) << std::endl;
        }

        // Test with malloc
        {
            std::cout << "\n[Testing malloc]" << std::endl;
            std::vector<void*> ptrs;

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < MIXED_ALLOCS; ++i)
            {
                size_t size = 8 << (i % 4); // 8, 16, 32, 64
                void* ptr = malloc(size);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: malloc failed at iteration " << i << std::endl;
                    return 1;
                }
                ptrs.push_back(ptr);

                if ((i + 1) % 12500 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << MIXED_ALLOCS << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;

            std::cout << "malloc time:      " << elapsed.count() << " s" << std::endl;
            std::cout << "Avg per alloc:    " << (elapsed.count() * 1e6 / MIXED_ALLOCS) << " us" << std::endl;
            std::cout << "Allocs per sec:   " << (MIXED_ALLOCS / elapsed.count()) << std::endl;

            // Free all malloc'd memory
            for (void* ptr : ptrs)
            {
                free(ptr);
            }
        }

        std::cout << "\n[PASSED] Test 3 completed\n" << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "[PASSED] All arena vs malloc tests passed!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
