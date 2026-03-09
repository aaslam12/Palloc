#include "pool.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace AL;

int main()
{
    const int POOL_BLOCKS = 1000000;     // 10K blocks
    const int CYCLES = 1000;           // 1K cycles
    const int ALLOCS_PER_CYCLE = 50000; // 5K allocations per cycle

    std::cout << "\n========================================" << '\n';
    std::cout << "Pool vs Malloc Performance Comparison" << '\n';
    std::cout << "========================================\n" << '\n';

    // ========================================================================
    // Test 1: Fixed-size allocation/free cycles
    // ========================================================================
    {
        std::cout << "--- Test 1: Fixed-Size Alloc/Free Cycles ---" << '\n';
        std::cout << "Block size:       64 bytes" << '\n';
        std::cout << "Cycles:           " << CYCLES << '\n';
        std::cout << "Allocs per cycle: " << ALLOCS_PER_CYCLE << '\n';

        // Test with Pool
        {
            std::cout << "\n[Testing Pool]" << '\n';
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
                        std::cerr << "ERROR: Pool allocation failed at cycle " << cycle << ", iteration " << i << '\n';
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
                    std::cout << "  Progress: " << (cycle + 1) << "/" << CYCLES << '\n';
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = CYCLES * ALLOCS_PER_CYCLE * 2; // alloc + free

            std::cout << "Pool time:        " << elapsed.count() << " s" << '\n';
            std::cout << "Total ops:        " << total_ops << " (allocs + frees)" << '\n';
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << '\n';
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << '\n';
        }

        // Test with malloc/free
        {
            std::cout << "\n[Testing malloc/free]" << '\n';

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
                        std::cerr << "ERROR: malloc failed at cycle " << cycle << ", iteration " << i << '\n';
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
                    std::cout << "  Progress: " << (cycle + 1) << "/" << CYCLES << '\n';
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = CYCLES * ALLOCS_PER_CYCLE * 2; // alloc + free

            std::cout << "malloc time:      " << elapsed.count() << " s" << '\n';
            std::cout << "Total ops:        " << total_ops << " (allocs + frees)" << '\n';
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << '\n';
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << '\n';
        }

        std::cout << "\n[PASSED] Test 1 completed\n" << '\n';
    }

    // ========================================================================
    // Test 2: Rapid allocation and immediate free
    // ========================================================================
    {
        std::cout << "--- Test 2: Rapid Alloc-Free Pairs ---" << '\n';
        const int RAPID_OPS = 1000000; // 1M alloc-free pairs
        std::cout << "Operations: " << RAPID_OPS << " alloc-free pairs" << '\n';
        std::cout << "Block size: 128 bytes" << '\n';

        // Test with Pool
        {
            std::cout << "\n[Testing Pool]" << '\n';
            AL::pool p(128, POOL_BLOCKS);

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < RAPID_OPS; ++i)
            {
                void* ptr = p.alloc();
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: Pool allocation failed at iteration " << i << '\n';
                    return 1;
                }
                p.free(ptr);

                if ((i + 1) % 250000 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << RAPID_OPS << '\n';
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = RAPID_OPS * 2; // alloc + free

            std::cout << "Pool time:        " << elapsed.count() << " s" << '\n';
            std::cout << "Total ops:        " << total_ops << '\n';
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << '\n';
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << '\n';
        }

        // Test with malloc/free
        {
            std::cout << "\n[Testing malloc/free]" << '\n';

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < RAPID_OPS; ++i)
            {
                void* ptr = malloc(128);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: malloc failed at iteration " << i << '\n';
                    return 1;
                }
                free(ptr);

                if ((i + 1) % 250000 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << RAPID_OPS << '\n';
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = RAPID_OPS * 2; // alloc + free

            std::cout << "malloc time:      " << elapsed.count() << " s" << '\n';
            std::cout << "Total ops:        " << total_ops << '\n';
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << '\n';
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << '\n';
        }

        std::cout << "\n[PASSED] Test 2 completed\n" << '\n';
    }

    // ========================================================================
    // Test 3: Full pool exhaustion and reuse
    // ========================================================================
    {
        std::cout << "--- Test 3: Full Pool Exhaustion and Reuse ---" << '\n';
        const int EXHAUSTION_CYCLES = 100;
        const int BLOCKS = 5000;
        std::cout << "Cycles:     " << EXHAUSTION_CYCLES << '\n';
        std::cout << "Blocks:     " << BLOCKS << '\n';
        std::cout << "Block size: 256 bytes" << '\n';

        // Test with Pool
        {
            std::cout << "\n[Testing Pool]" << '\n';
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
                        std::cerr << "ERROR: Pool exhausted prematurely at cycle " << cycle << ", block " << i << '\n';
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
                    std::cout << "  Progress: " << (cycle + 1) << "/" << EXHAUSTION_CYCLES << '\n';
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = EXHAUSTION_CYCLES * BLOCKS * 2; // alloc + free

            std::cout << "Pool time:        " << elapsed.count() << " s" << '\n';
            std::cout << "Total ops:        " << total_ops << '\n';
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << '\n';
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << '\n';
        }

        // Test with malloc/free
        {
            std::cout << "\n[Testing malloc/free]" << '\n';

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
                        std::cerr << "ERROR: malloc failed at cycle " << cycle << ", block " << i << '\n';
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
                    std::cout << "  Progress: " << (cycle + 1) << "/" << EXHAUSTION_CYCLES << '\n';
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = EXHAUSTION_CYCLES * BLOCKS * 2; // alloc + free

            std::cout << "malloc time:      " << elapsed.count() << " s" << '\n';
            std::cout << "Total ops:        " << total_ops << '\n';
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << '\n';
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << '\n';
        }

        std::cout << "\n[PASSED] Test 3 completed\n" << '\n';
    }

    std::cout << "========================================" << '\n';
    std::cout << "[PASSED] All pool vs malloc tests passed!" << '\n';
    std::cout << "========================================" << '\n';

    return 0;
}
