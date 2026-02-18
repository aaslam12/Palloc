#include "pool.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace AL;

int main()
{
    const int POOL_BLOCKS = 10000;         // 10K blocks
    const int CYCLES = 1000;               // 1K cycles
    const int ALLOCS_PER_CYCLE = 5000;     // 5K allocations per cycle

    std::cout << "\n========================================" << std::endl;
    std::cout << "Pool vs Malloc Performance Comparison" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // ========================================================================
    // Test 1: Fixed-size allocation/free cycles
    // ========================================================================
    {
        std::cout << "--- Test 1: Fixed-Size Alloc/Free Cycles ---" << std::endl;
        std::cout << "Block size:       64 bytes" << std::endl;
        std::cout << "Cycles:           " << CYCLES << std::endl;
        std::cout << "Allocs per cycle: " << ALLOCS_PER_CYCLE << std::endl;

        // Test with Pool
        {
            std::cout << "\n[Testing Pool]" << std::endl;
            AL::pool p(64, POOL_BLOCKS);

            auto start = std::chrono::high_resolution_clock::now();

            for (int cycle = 0; cycle < CYCLES; ++cycle)
            {
                std::vector<void*> ptrs;

                // Allocate
                for (int i = 0; i < ALLOCS_PER_CYCLE; ++i)
                {
                    void* ptr = p.alloc();
                    if (ptr == nullptr)
                    {
                        std::cerr << "ERROR: Pool allocation failed at cycle " << cycle
                                  << ", iteration " << i << std::endl;
                        return 1;
                    }
                    ptrs.push_back(ptr);
                }

                // Free
                for (void* ptr : ptrs)
                {
                    p.free(ptr);
                }

                if ((cycle + 1) % 250 == 0)
                {
                    std::cout << "  Progress: " << (cycle + 1) << "/" << CYCLES << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = CYCLES * ALLOCS_PER_CYCLE * 2; // alloc + free

            std::cout << "Pool time:        " << elapsed.count() << " s" << std::endl;
            std::cout << "Total ops:        " << total_ops << " (allocs + frees)" << std::endl;
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << std::endl;
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << std::endl;
        }

        // Test with malloc/free
        {
            std::cout << "\n[Testing malloc/free]" << std::endl;

            auto start = std::chrono::high_resolution_clock::now();

            for (int cycle = 0; cycle < CYCLES; ++cycle)
            {
                std::vector<void*> ptrs;

                // Allocate
                for (int i = 0; i < ALLOCS_PER_CYCLE; ++i)
                {
                    void* ptr = malloc(64);
                    if (ptr == nullptr)
                    {
                        std::cerr << "ERROR: malloc failed at cycle " << cycle
                                  << ", iteration " << i << std::endl;
                        return 1;
                    }
                    ptrs.push_back(ptr);
                }

                // Free
                for (void* ptr : ptrs)
                {
                    free(ptr);
                }

                if ((cycle + 1) % 250 == 0)
                {
                    std::cout << "  Progress: " << (cycle + 1) << "/" << CYCLES << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = CYCLES * ALLOCS_PER_CYCLE * 2; // alloc + free

            std::cout << "malloc time:      " << elapsed.count() << " s" << std::endl;
            std::cout << "Total ops:        " << total_ops << " (allocs + frees)" << std::endl;
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << std::endl;
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << std::endl;
        }

        std::cout << "\n[PASSED] Test 1 completed\n" << std::endl;
    }

    // ========================================================================
    // Test 2: Rapid allocation and immediate free
    // ========================================================================
    {
        std::cout << "--- Test 2: Rapid Alloc-Free Pairs ---" << std::endl;
        const int RAPID_OPS = 1000000; // 1M alloc-free pairs
        std::cout << "Operations: " << RAPID_OPS << " alloc-free pairs" << std::endl;
        std::cout << "Block size: 128 bytes" << std::endl;

        // Test with Pool
        {
            std::cout << "\n[Testing Pool]" << std::endl;
            AL::pool p(128, POOL_BLOCKS);

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < RAPID_OPS; ++i)
            {
                void* ptr = p.alloc();
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: Pool allocation failed at iteration " << i << std::endl;
                    return 1;
                }
                p.free(ptr);

                if ((i + 1) % 250000 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << RAPID_OPS << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = RAPID_OPS * 2; // alloc + free

            std::cout << "Pool time:        " << elapsed.count() << " s" << std::endl;
            std::cout << "Total ops:        " << total_ops << std::endl;
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << std::endl;
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << std::endl;
        }

        // Test with malloc/free
        {
            std::cout << "\n[Testing malloc/free]" << std::endl;

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < RAPID_OPS; ++i)
            {
                void* ptr = malloc(128);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: malloc failed at iteration " << i << std::endl;
                    return 1;
                }
                free(ptr);

                if ((i + 1) % 250000 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << RAPID_OPS << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = RAPID_OPS * 2; // alloc + free

            std::cout << "malloc time:      " << elapsed.count() << " s" << std::endl;
            std::cout << "Total ops:        " << total_ops << std::endl;
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << std::endl;
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << std::endl;
        }

        std::cout << "\n[PASSED] Test 2 completed\n" << std::endl;
    }

    // ========================================================================
    // Test 3: Full pool exhaustion and reuse
    // ========================================================================
    {
        std::cout << "--- Test 3: Full Pool Exhaustion and Reuse ---" << std::endl;
        const int EXHAUSTION_CYCLES = 100;
        const int BLOCKS = 5000;
        std::cout << "Cycles:     " << EXHAUSTION_CYCLES << std::endl;
        std::cout << "Blocks:     " << BLOCKS << std::endl;
        std::cout << "Block size: 256 bytes" << std::endl;

        // Test with Pool
        {
            std::cout << "\n[Testing Pool]" << std::endl;
            AL::pool p(256, BLOCKS);

            auto start = std::chrono::high_resolution_clock::now();

            for (int cycle = 0; cycle < EXHAUSTION_CYCLES; ++cycle)
            {
                std::vector<void*> ptrs;

                // Exhaust the pool
                for (int i = 0; i < BLOCKS; ++i)
                {
                    void* ptr = p.alloc();
                    if (ptr == nullptr)
                    {
                        std::cerr << "ERROR: Pool exhausted prematurely at cycle " << cycle
                                  << ", block " << i << std::endl;
                        return 1;
                    }
                    ptrs.push_back(ptr);
                }

                // Free all blocks
                for (void* ptr : ptrs)
                {
                    p.free(ptr);
                }

                if ((cycle + 1) % 25 == 0)
                {
                    std::cout << "  Progress: " << (cycle + 1) << "/" << EXHAUSTION_CYCLES << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = EXHAUSTION_CYCLES * BLOCKS * 2; // alloc + free

            std::cout << "Pool time:        " << elapsed.count() << " s" << std::endl;
            std::cout << "Total ops:        " << total_ops << std::endl;
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << std::endl;
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << std::endl;
        }

        // Test with malloc/free
        {
            std::cout << "\n[Testing malloc/free]" << std::endl;

            auto start = std::chrono::high_resolution_clock::now();

            for (int cycle = 0; cycle < EXHAUSTION_CYCLES; ++cycle)
            {
                std::vector<void*> ptrs;

                // Allocate same number of blocks
                for (int i = 0; i < BLOCKS; ++i)
                {
                    void* ptr = malloc(256);
                    if (ptr == nullptr)
                    {
                        std::cerr << "ERROR: malloc failed at cycle " << cycle
                                  << ", block " << i << std::endl;
                        return 1;
                    }
                    ptrs.push_back(ptr);
                }

                // Free all blocks
                for (void* ptr : ptrs)
                {
                    free(ptr);
                }

                if ((cycle + 1) % 25 == 0)
                {
                    std::cout << "  Progress: " << (cycle + 1) << "/" << EXHAUSTION_CYCLES << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = EXHAUSTION_CYCLES * BLOCKS * 2; // alloc + free

            std::cout << "malloc time:      " << elapsed.count() << " s" << std::endl;
            std::cout << "Total ops:        " << total_ops << std::endl;
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << std::endl;
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << std::endl;
        }

        std::cout << "\n[PASSED] Test 3 completed\n" << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "[PASSED] All pool vs malloc tests passed!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
