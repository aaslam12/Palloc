#include "arena.h"
#include <chrono>
#include <iostream>
#include <unistd.h>
#include <vector>

using namespace AL;

static const size_t PAGE_SIZE = getpagesize();

int main()
{
    const int SMALL_ALLOCS = 100000;       // 100K allocations (was 1K)
    const int RESET_CYCLES = 10000;        // 10K cycles (was 100)
    const int ALLOCS_PER_RESET = 100;      // 100 per reset (was 10)
    const int ALLOC_SIZE = 100;

    std::cout << "\n=== Arena Allocator Stress Test ===" << std::endl;
    std::cout << "Page size: " << PAGE_SIZE << " bytes\n" << std::endl;

    // ========================================================================
    // Test 1: Many small allocations
    // ========================================================================
    {
        std::cout << "--- Test 1: Many Small Allocations ---" << std::endl;
        std::cout << "Arena size:   " << (PAGE_SIZE * 1000) << " bytes (" << (PAGE_SIZE * 1000 / 1024) << " KB)" << std::endl;
        std::cout << "Allocations:  " << SMALL_ALLOCS << " x 8 bytes" << std::endl;

        AL::arena a(PAGE_SIZE * 1000);
        std::vector<void*> ptrs;

        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < SMALL_ALLOCS; ++i)
        {
            void* ptr = a.alloc(8);
            if (ptr == nullptr)
            {
                std::cerr << "ERROR: Failed to allocate at iteration " << i << std::endl;
                return 1;
            }
            ptrs.push_back(ptr);

            // Progress indicator
            if ((i + 1) % 20000 == 0)
            {
                std::cout << "  Progress: " << (i + 1) << "/" << SMALL_ALLOCS
                          << " allocations, used=" << a.get_used() << " bytes" << std::endl;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = end - start;

        std::cout << "\n[Test 1 Results]" << std::endl;
        std::cout << "Total time:       " << diff.count() << " s" << std::endl;
        std::cout << "Allocations:      " << ptrs.size() << std::endl;
        std::cout << "Avg per alloc:    " << (diff.count() * 1e6 / ptrs.size()) << " us" << std::endl;
        std::cout << "Allocs per sec:   " << (ptrs.size() / diff.count()) << std::endl;
        std::cout << "Bytes used:       " << a.get_used() << " / " << a.get_capacity() << std::endl;
        std::cout << "Utilization:      " << (100.0 * a.get_used() / a.get_capacity()) << "%" << std::endl;

        // Sanity check
        if (ptrs.size() == 0)
        {
            std::cerr << "ERROR: Failed to allocate any blocks" << std::endl;
            return 1;
        }
        if (a.get_used() != ptrs.size() * 8)
        {
            std::cerr << "ERROR: Used size doesn't match allocations! Expected "
                      << (ptrs.size() * 8) << ", got " << a.get_used() << std::endl;
            return 1;
        }

        std::cout << "Sanity check:     PASSED (usage tracking correct)" << std::endl;
        std::cout << "[PASSED] Test 1: Many small allocations\n" << std::endl;
    }

    // ========================================================================
    // Test 2: Multiple cycles of use and reset
    // ========================================================================
    {
        std::cout << "--- Test 2: Repeated Alloc/Reset Cycles ---" << std::endl;
        std::cout << "Arena size:       " << (PAGE_SIZE * 4) << " bytes" << std::endl;
        std::cout << "Cycles:           " << RESET_CYCLES << std::endl;
        std::cout << "Allocs per cycle: " << ALLOCS_PER_RESET << " x " << ALLOC_SIZE << " bytes" << std::endl;

        AL::arena a(PAGE_SIZE * 4);

        auto start = std::chrono::high_resolution_clock::now();

        for (int cycle = 0; cycle < RESET_CYCLES; ++cycle)
        {
            std::vector<void*> ptrs;

            // Allocate several blocks
            for (int i = 0; i < ALLOCS_PER_RESET; ++i)
            {
                void* ptr = a.alloc(ALLOC_SIZE);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: Failed to allocate at cycle " << cycle
                              << ", iteration " << i << std::endl;
                    return 1;
                }
                ptrs.push_back(ptr);
            }

            if (a.get_used() != ALLOCS_PER_RESET * ALLOC_SIZE)
            {
                std::cerr << "ERROR: Used space not tracked correctly in cycle " << cycle
                          << ". Expected " << (ALLOCS_PER_RESET * ALLOC_SIZE)
                          << ", got " << a.get_used() << std::endl;
                return 1;
            }

            // Reset
            int result = a.reset();
            if (result != 0)
            {
                std::cerr << "ERROR: Reset failed in cycle " << cycle << std::endl;
                return 1;
            }
            if (a.get_used() != 0)
            {
                std::cerr << "ERROR: Reset didn't clear used space in cycle " << cycle << std::endl;
                return 1;
            }

            // Progress indicator
            if ((cycle + 1) % 2500 == 0)
            {
                std::cout << "  Progress: " << (cycle + 1) << "/" << RESET_CYCLES
                          << " cycles completed" << std::endl;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = end - start;

        int total_ops = RESET_CYCLES * (ALLOCS_PER_RESET + 1); // allocs + reset

        std::cout << "\n[Test 2 Results]" << std::endl;
        std::cout << "Total time:       " << diff.count() << " s" << std::endl;
        std::cout << "Total operations: " << total_ops << " (" << ALLOCS_PER_RESET << " allocs + 1 reset per cycle)" << std::endl;
        std::cout << "Avg per cycle:    " << (diff.count() * 1e6 / RESET_CYCLES) << " us" << std::endl;
        std::cout << "Cycles per sec:   " << (RESET_CYCLES / diff.count()) << std::endl;

        // Sanity check
        if (a.get_used() != 0)
        {
            std::cerr << "ERROR: Arena not reset! Used space: " << a.get_used() << std::endl;
            return 1;
        }

        std::cout << "Sanity check:     PASSED (arena properly reset)" << std::endl;
        std::cout << "[PASSED] Test 2: Repeated alloc/reset cycles\n" << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "[PASSED] All arena stress tests passed!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
