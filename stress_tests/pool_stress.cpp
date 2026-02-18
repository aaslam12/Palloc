#include "pool.h"
#include <chrono>
#include <iostream>
#include <vector>

using namespace AL;

int main()
{
    const int BLOCK_SIZE = 128;
    const int BLOCK_COUNT = 10000;         // 10K blocks (was 1K)
    const int NUM_CYCLES = 1000;           // 1K cycles (was 100)
    const int ALLOCS_PER_CYCLE = 5000;     // 5K per cycle (was 500)
    const int FULL_CYCLES = 100;           // 100 cycles (was 10)

    std::cout << "\n=== Pool Allocator Stress Test ===" << std::endl;
    std::cout << "Pool configuration: " << BLOCK_SIZE << " byte blocks, "
              << BLOCK_COUNT << " blocks\n" << std::endl;

    AL::pool p(BLOCK_SIZE, BLOCK_COUNT);

    // ========================================================================
    // Test 1: Many alloc/free cycles (partial pool usage)
    // ========================================================================
    {
        std::cout << "--- Test 1: Partial Pool Cycles ---" << std::endl;
        std::cout << "Cycles:           " << NUM_CYCLES << std::endl;
        std::cout << "Allocs per cycle: " << ALLOCS_PER_CYCLE << " (50% of pool)" << std::endl;

        auto start = std::chrono::high_resolution_clock::now();

        for (int cycle = 0; cycle < NUM_CYCLES; ++cycle)
        {
            std::vector<void*> ptrs;

            // Allocate half the pool
            for (int i = 0; i < ALLOCS_PER_CYCLE; ++i)
            {
                void* ptr = p.alloc();
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: Failed to allocate at cycle " << cycle
                              << ", iteration " << i << std::endl;
                    return 1;
                }
                ptrs.push_back(ptr);
            }

            // Free all
            for (void* ptr : ptrs)
            {
                p.free(ptr);
            }

            // Progress indicator
            if ((cycle + 1) % 250 == 0)
            {
                std::cout << "  Progress: " << (cycle + 1) << "/" << NUM_CYCLES
                          << " cycles completed" << std::endl;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = end - start;

        int total_ops = NUM_CYCLES * ALLOCS_PER_CYCLE * 2; // alloc + free

        std::cout << "\n[Test 1 Results]" << std::endl;
        std::cout << "Total time:       " << diff.count() << " s" << std::endl;
        std::cout << "Total operations: " << total_ops << " (alloc + free)" << std::endl;
        std::cout << "Avg per op:       " << (diff.count() * 1e6 / total_ops) << " us" << std::endl;
        std::cout << "Ops per second:   " << (total_ops / diff.count()) << std::endl;

        // Sanity check
        if (p.get_free_space() != BLOCK_SIZE * BLOCK_COUNT)
        {
            std::cerr << "ERROR: Pool free space not restored! Expected "
                      << (BLOCK_SIZE * BLOCK_COUNT) << ", got " << p.get_free_space() << std::endl;
            return 1;
        }

        std::cout << "Sanity check:     PASSED (all blocks freed)" << std::endl;
        std::cout << "[PASSED] Test 1: Partial pool cycles\n" << std::endl;
    }

    // ========================================================================
    // Test 2: Allocate all, free all, repeat
    // ========================================================================
    {
        std::cout << "--- Test 2: Full Pool Exhaustion Cycles ---" << std::endl;
        std::cout << "Cycles:           " << FULL_CYCLES << std::endl;
        std::cout << "Allocs per cycle: " << BLOCK_COUNT << " (100% of pool)" << std::endl;

        auto start = std::chrono::high_resolution_clock::now();

        for (int cycle = 0; cycle < FULL_CYCLES; ++cycle)
        {
            std::vector<void*> ptrs;

            // Fill completely
            for (int i = 0; i < BLOCK_COUNT; ++i)
            {
                void* ptr = p.alloc();
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: Failed to allocate at cycle " << cycle
                              << ", block " << i << std::endl;
                    return 1;
                }
                ptrs.push_back(ptr);
            }

            // Verify pool is exhausted
            void* should_fail = p.alloc();
            if (should_fail != nullptr)
            {
                std::cerr << "ERROR: Pool should be exhausted but allocation succeeded!" << std::endl;
                return 1;
            }

            // Free all
            for (void* ptr : ptrs)
            {
                p.free(ptr);
            }

            std::cout << "  Cycle " << (cycle + 1) << "/" << FULL_CYCLES << " completed" << std::endl;
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = end - start;

        int total_ops = FULL_CYCLES * BLOCK_COUNT * 2; // alloc + free

        std::cout << "\n[Test 2 Results]" << std::endl;
        std::cout << "Total time:       " << diff.count() << " s" << std::endl;
        std::cout << "Total operations: " << total_ops << " (alloc + free)" << std::endl;
        std::cout << "Avg per op:       " << (diff.count() * 1e6 / total_ops) << " us" << std::endl;
        std::cout << "Ops per second:   " << (total_ops / diff.count()) << std::endl;

        // Sanity check
        if (p.get_free_space() != BLOCK_SIZE * BLOCK_COUNT)
        {
            std::cerr << "ERROR: Pool free space not restored! Expected "
                      << (BLOCK_SIZE * BLOCK_COUNT) << ", got " << p.get_free_space() << std::endl;
            return 1;
        }

        std::cout << "Sanity check:     PASSED (all blocks freed)" << std::endl;
        std::cout << "[PASSED] Test 2: Full pool exhaustion cycles\n" << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "[PASSED] All pool stress tests passed!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
