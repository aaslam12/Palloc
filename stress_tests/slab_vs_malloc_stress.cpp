#include "slab.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace AL;

int main()
{
    const int MIXED_CYCLES = 10000;        // 10K cycles
    const int ALLOCS_PER_CYCLE = 100;      // 100 allocations per cycle
    const int RAPID_OPS = 1000000;         // 1M rapid operations

    std::cout << "\n========================================" << std::endl;
    std::cout << "Slab vs Malloc Performance Comparison" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // ========================================================================
    // Test 1: Mixed size allocations with varying patterns
    // ========================================================================
    {
        std::cout << "--- Test 1: Mixed Size Allocations ---" << std::endl;
        std::cout << "Cycles:           " << MIXED_CYCLES << std::endl;
        std::cout << "Allocs per cycle: " << ALLOCS_PER_CYCLE << std::endl;
        std::cout << "Sizes:            32, 64, 128, 256 bytes (rotating)" << std::endl;

        // Test with Slab
        {
            std::cout << "\n[Testing Slab]" << std::endl;
            AL::slab s(1.0);

            auto start = std::chrono::high_resolution_clock::now();

            for (int cycle = 0; cycle < MIXED_CYCLES; ++cycle)
            {
                std::vector<std::pair<void*, size_t>> ptrs;

                // Allocate with rotating sizes
                for (int i = 0; i < ALLOCS_PER_CYCLE; ++i)
                {
                    size_t size;
                    switch (i % 4)
                    {
                        case 0: size = 32; break;
                        case 1: size = 64; break;
                        case 2: size = 128; break;
                        case 3: size = 256; break;
                        default: size = 64;
                    }

                    void* ptr = s.alloc(size);
                    if (ptr == nullptr)
                    {
                        std::cerr << "ERROR: Slab allocation failed at cycle " << cycle
                                  << ", iteration " << i << ", size " << size << std::endl;
                        return 1;
                    }
                    ptrs.push_back({ptr, size});
                }

                // Free all
                for (auto [ptr, size] : ptrs)
                {
                    s.free(ptr, size);
                }

                if ((cycle + 1) % 2500 == 0)
                {
                    std::cout << "  Progress: " << (cycle + 1) << "/" << MIXED_CYCLES << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = MIXED_CYCLES * ALLOCS_PER_CYCLE * 2; // alloc + free

            std::cout << "Slab time:        " << elapsed.count() << " s" << std::endl;
            std::cout << "Total ops:        " << total_ops << " (allocs + frees)" << std::endl;
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << std::endl;
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << std::endl;
        }

        // Test with malloc/free
        {
            std::cout << "\n[Testing malloc/free]" << std::endl;

            auto start = std::chrono::high_resolution_clock::now();

            for (int cycle = 0; cycle < MIXED_CYCLES; ++cycle)
            {
                std::vector<void*> ptrs;

                // Allocate with rotating sizes
                for (int i = 0; i < ALLOCS_PER_CYCLE; ++i)
                {
                    size_t size;
                    switch (i % 4)
                    {
                        case 0: size = 32; break;
                        case 1: size = 64; break;
                        case 2: size = 128; break;
                        case 3: size = 256; break;
                        default: size = 64;
                    }

                    void* ptr = malloc(size);
                    if (ptr == nullptr)
                    {
                        std::cerr << "ERROR: malloc failed at cycle " << cycle
                                  << ", iteration " << i << ", size " << size << std::endl;
                        return 1;
                    }
                    ptrs.push_back(ptr);
                }

                // Free all
                for (void* ptr : ptrs)
                {
                    free(ptr);
                }

                if ((cycle + 1) % 2500 == 0)
                {
                    std::cout << "  Progress: " << (cycle + 1) << "/" << MIXED_CYCLES << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = MIXED_CYCLES * ALLOCS_PER_CYCLE * 2; // alloc + free

            std::cout << "malloc time:      " << elapsed.count() << " s" << std::endl;
            std::cout << "Total ops:        " << total_ops << " (allocs + frees)" << std::endl;
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << std::endl;
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << std::endl;
        }

        std::cout << "\n[PASSED] Test 1 completed\n" << std::endl;
    }

    // ========================================================================
    // Test 2: Rapid single-size allocations
    // ========================================================================
    {
        std::cout << "--- Test 2: Rapid Single-Size Allocations ---" << std::endl;
        std::cout << "Operations: " << RAPID_OPS << std::endl;
        std::cout << "Size:       64 bytes" << std::endl;
        std::cout << "Pattern:    Allocate immediately followed by free" << std::endl;

        // Test with Slab
        {
            std::cout << "\n[Testing Slab]" << std::endl;
            AL::slab s(1.0);

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < RAPID_OPS; ++i)
            {
                void* ptr = s.alloc(64);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: Slab allocation failed at iteration " << i << std::endl;
                    return 1;
                }
                s.free(ptr, 64);

                if ((i + 1) % 250000 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << RAPID_OPS << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = RAPID_OPS * 2; // alloc + free

            std::cout << "Slab time:        " << elapsed.count() << " s" << std::endl;
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
                void* ptr = malloc(64);
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
    // Test 3: Small allocation pattern (common use case)
    // ========================================================================
    {
        std::cout << "--- Test 3: Small Allocation Pattern ---" << std::endl;
        const int SMALL_OPS = 500000; // 500K operations
        std::cout << "Operations: " << SMALL_OPS << std::endl;
        std::cout << "Sizes:      8, 16, 24, 32 bytes (realistic small objects)" << std::endl;

        // Test with Slab
        {
            std::cout << "\n[Testing Slab]" << std::endl;
            AL::slab s(1.0);

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < SMALL_OPS; ++i)
            {
                size_t size = 8 + (i % 4) * 8; // 8, 16, 24, 32
                void* ptr = s.alloc(size);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: Slab allocation failed at iteration " << i
                              << ", size " << size << std::endl;
                    return 1;
                }
                s.free(ptr, size);

                if ((i + 1) % 125000 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << SMALL_OPS << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = SMALL_OPS * 2; // alloc + free

            std::cout << "Slab time:        " << elapsed.count() << " s" << std::endl;
            std::cout << "Total ops:        " << total_ops << std::endl;
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << std::endl;
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << std::endl;
        }

        // Test with malloc/free
        {
            std::cout << "\n[Testing malloc/free]" << std::endl;

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < SMALL_OPS; ++i)
            {
                size_t size = 8 + (i % 4) * 8; // 8, 16, 24, 32
                void* ptr = malloc(size);
                if (ptr == nullptr)
                {
                    std::cerr << "ERROR: malloc failed at iteration " << i
                              << ", size " << size << std::endl;
                    return 1;
                }
                free(ptr);

                if ((i + 1) % 125000 == 0)
                {
                    std::cout << "  Progress: " << (i + 1) << "/" << SMALL_OPS << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = SMALL_OPS * 2; // alloc + free

            std::cout << "malloc time:      " << elapsed.count() << " s" << std::endl;
            std::cout << "Total ops:        " << total_ops << std::endl;
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << std::endl;
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << std::endl;
        }

        std::cout << "\n[PASSED] Test 3 completed\n" << std::endl;
    }

    // ========================================================================
    // Test 4: Batch allocation with delayed free
    // ========================================================================
    {
        std::cout << "--- Test 4: Batch Allocation with Delayed Free ---" << std::endl;
        const int BATCH_SIZE = 10000;
        const int BATCHES = 100;
        std::cout << "Batches:          " << BATCHES << std::endl;
        std::cout << "Allocs per batch: " << BATCH_SIZE << std::endl;
        std::cout << "Sizes:            16, 32, 64, 128 bytes" << std::endl;

        // Test with Slab (use larger scale for this test)
        {
            std::cout << "\n[Testing Slab]" << std::endl;
            AL::slab s(20.0); // Large scale to handle 10K batch with 25% being 128-byte (2.5K blocks needed)

            auto start = std::chrono::high_resolution_clock::now();

            for (int batch = 0; batch < BATCHES; ++batch)
            {
                std::vector<std::pair<void*, size_t>> ptrs;

                // Allocate entire batch
                for (int i = 0; i < BATCH_SIZE; ++i)
                {
                    size_t size;
                    switch (i % 4)
                    {
                        case 0: size = 16; break;
                        case 1: size = 32; break;
                        case 2: size = 64; break;
                        case 3: size = 128; break;
                        default: size = 32;
                    }

                    void* ptr = s.alloc(size);
                    if (ptr == nullptr)
                    {
                        std::cerr << "ERROR: Slab allocation failed at batch " << batch
                                  << ", iteration " << i << ", size " << size << std::endl;
                        return 1;
                    }
                    ptrs.push_back({ptr, size});
                }

                // Free entire batch
                for (auto [ptr, size] : ptrs)
                {
                    s.free(ptr, size);
                }

                if ((batch + 1) % 25 == 0)
                {
                    std::cout << "  Progress: " << (batch + 1) << "/" << BATCHES << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = BATCHES * BATCH_SIZE * 2; // alloc + free

            std::cout << "Slab time:        " << elapsed.count() << " s" << std::endl;
            std::cout << "Total ops:        " << total_ops << std::endl;
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << std::endl;
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << std::endl;
        }

        // Test with malloc/free
        {
            std::cout << "\n[Testing malloc/free]" << std::endl;

            auto start = std::chrono::high_resolution_clock::now();

            for (int batch = 0; batch < BATCHES; ++batch)
            {
                std::vector<void*> ptrs;

                // Allocate entire batch
                for (int i = 0; i < BATCH_SIZE; ++i)
                {
                    size_t size;
                    switch (i % 4)
                    {
                        case 0: size = 16; break;
                        case 1: size = 32; break;
                        case 2: size = 64; break;
                        case 3: size = 128; break;
                        default: size = 32;
                    }

                    void* ptr = malloc(size);
                    if (ptr == nullptr)
                    {
                        std::cerr << "ERROR: malloc failed at batch " << batch
                                  << ", iteration " << i << ", size " << size << std::endl;
                        return 1;
                    }
                    ptrs.push_back(ptr);
                }

                // Free entire batch
                for (void* ptr : ptrs)
                {
                    free(ptr);
                }

                if ((batch + 1) % 25 == 0)
                {
                    std::cout << "  Progress: " << (batch + 1) << "/" << BATCHES << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            int total_ops = BATCHES * BATCH_SIZE * 2; // alloc + free

            std::cout << "malloc time:      " << elapsed.count() << " s" << std::endl;
            std::cout << "Total ops:        " << total_ops << std::endl;
            std::cout << "Avg per op:       " << (elapsed.count() * 1e6 / total_ops) << " us" << std::endl;
            std::cout << "Ops per sec:      " << (total_ops / elapsed.count()) << std::endl;
        }

        std::cout << "\n[PASSED] Test 4 completed\n" << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "[PASSED] All slab vs malloc tests passed!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
