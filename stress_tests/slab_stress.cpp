#include "slab.h"
#include <chrono>
#include <iostream>
#include <vector>

using namespace AL;

int main()
{
    const int NUM_CYCLES = 10000;          // 10K cycles (was 100)
    const int ALLOCS_PER_CYCLE = 100;      // 100 per cycle (was 20)
    const int RAPID_CYCLES = 1000000;      // 1M rapid cycles (was 10K)

    std::cout << "\n=== Slab Allocator Stress Test ===" << std::endl;
    std::cout << "Testing slab allocator under various stress patterns\n" << std::endl;

    // ========================================================================
    // Test 1: Many cycles with mixed sizes
    // ========================================================================
    {
        std::cout << "--- Test 1: Mixed Size Allocations ---" << std::endl;
        std::cout << "Cycles:           " << NUM_CYCLES << std::endl;
        std::cout << "Allocs per cycle: " << ALLOCS_PER_CYCLE << std::endl;
        std::cout << "Sizes:            32, 64, 128, 256 bytes (rotating)" << std::endl;

        AL::slab s;
        size_t initial_free = s.get_total_free();

        auto start = std::chrono::high_resolution_clock::now();

        for (int cycle = 0; cycle < NUM_CYCLES; ++cycle)
        {
            std::vector<std::pair<void*, size_t>> ptrs;

            // Allocate phase
            for (int i = 0; i < ALLOCS_PER_CYCLE; ++i)
            {
                size_t size = (i % 4 == 0) ? 32 : (i % 4 == 1) ? 64
                                                  : (i % 4 == 2)    ? 128
                                                                     : 256;
                void* ptr = s.alloc(size);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: Failed to allocate " << size << " bytes at cycle "
                              << cycle << ", iteration " << i << std::endl;
                    return 1;
                }
                ptrs.push_back({ptr, size});
            }

            // Free phase
            for (auto& [ptr, size] : ptrs)
                s.free(ptr, size);

            // Progress indicator
            if ((cycle + 1) % 2500 == 0)
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
        if (s.get_total_free() != initial_free)
        {
            std::cerr << "ERROR: Free space not restored! Expected " << initial_free
                      << ", got " << s.get_total_free() << std::endl;
            return 1;
        }

        std::cout << "Sanity check:     PASSED (all memory freed)" << std::endl;
        std::cout << "[PASSED] Test 1: Mixed size allocations\n" << std::endl;
    }

    // ========================================================================
    // Test 2: Rapid alloc/free single size
    // ========================================================================
    {
        std::cout << "--- Test 2: Rapid Single-Size Allocations ---" << std::endl;
        std::cout << "Operations:  " << RAPID_CYCLES << std::endl;
        std::cout << "Size:        64 bytes" << std::endl;
        std::cout << "Pattern:     Allocate immediately followed by free" << std::endl;

        AL::slab s;
        size_t initial_free = s.get_total_free();

        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < RAPID_CYCLES; ++i)
        {
            void* ptr = s.alloc(64);
            if (ptr == nullptr)
            {
                std::cerr << "ERROR: Failed to allocate at iteration " << i << std::endl;
                return 1;
            }
            s.free(ptr, 64);

            // Progress indicator
            if ((i + 1) % 200000 == 0)
            {
                std::cout << "  Progress: " << (i + 1) << "/" << RAPID_CYCLES
                          << " cycles completed" << std::endl;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = end - start;

        int total_ops = RAPID_CYCLES * 2; // alloc + free

        std::cout << "\n[Test 2 Results]" << std::endl;
        std::cout << "Total time:       " << diff.count() << " s" << std::endl;
        std::cout << "Total operations: " << total_ops << " (alloc + free)" << std::endl;
        std::cout << "Avg per op:       " << (diff.count() * 1e6 / total_ops) << " us" << std::endl;
        std::cout << "Ops per second:   " << (total_ops / diff.count()) << std::endl;

        // Sanity check
        if (s.get_total_free() != initial_free)
        {
            std::cerr << "ERROR: Free space not restored! Expected " << initial_free
                      << ", got " << s.get_total_free() << std::endl;
            return 1;
        }

        std::cout << "Sanity check:     PASSED (all memory freed)" << std::endl;
        std::cout << "[PASSED] Test 2: Rapid single-size allocations\n" << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "[PASSED] All slab stress tests passed!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}

