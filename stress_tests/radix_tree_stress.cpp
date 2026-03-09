#include "radix_tree.h"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

static void* addr(uintptr_t v)
{
    return reinterpret_cast<void*>(v);
}

int main()
{
    const size_t NUM_RANGES = 1000000;
    const uintptr_t RANGE_SIZE = 0x1000;
    const uintptr_t GAP = 0x1000;
    const size_t LOOKUPS_PER_TEST = 10000000;

    std::cout << "\n=== Radix Tree Stress Test ===" << '\n';
    std::cout << "Ranges: " << NUM_RANGES << '\n';
    std::cout << "Range size: " << RANGE_SIZE << " bytes" << '\n';
    std::cout << "Lookups per test: " << LOOKUPS_PER_TEST << "\n" << '\n';

    AL::radix_tree rt;

    struct range_info
    {
        uintptr_t start;
        uintptr_t end;
        size_t slab_id;
    };

    std::vector<range_info> ranges;
    ranges.reserve(NUM_RANGES);

    // ========================================================================
    // Test 1: Bulk insertion throughput
    // ========================================================================
    {
        std::cout << "--- Test 1: Bulk Insertion ---" << '\n';

        auto start = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < NUM_RANGES; ++i)
        {
            uintptr_t range_start = 0x100000 + i * (RANGE_SIZE + GAP);
            uintptr_t range_end = range_start + RANGE_SIZE;
            rt.insert(addr(range_start), addr(range_end), i + 1);
            ranges.push_back({range_start, range_end, i + 1});
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

        std::cout << "Total: " << ns / 1000 << " us" << '\n';
        std::cout << "Per insert: " << ns / NUM_RANGES << " ns\n" << '\n';
    }

    // ========================================================================
    // Test 2: Sequential lookup throughput (known hits)
    // ========================================================================
    {
        std::cout << "--- Test 2: Sequential Lookup (Hits) ---" << '\n';

        size_t found = 0;
        auto start = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < LOOKUPS_PER_TEST; ++i)
        {
            size_t idx = i % NUM_RANGES;
            uintptr_t target = ranges[idx].start + RANGE_SIZE / 2;
            size_t result = rt.lookup(addr(target));
            if (result != 0)
                ++found;
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

        std::cout << "Total: " << ns / 1000 << " us" << '\n';
        std::cout << "Per lookup: " << ns / LOOKUPS_PER_TEST << " ns" << '\n';
        std::cout << "Hit rate: " << (found * 100 / LOOKUPS_PER_TEST) << "%\n" << '\n';
    }

    // ========================================================================
    // Test 3: Random lookup throughput (hits)
    // ========================================================================
    {
        std::cout << "--- Test 3: Random Lookup (Hits) ---" << '\n';

        std::mt19937_64 rng(42);
        size_t found = 0;
        auto start = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < LOOKUPS_PER_TEST; ++i)
        {
            size_t idx = rng() % NUM_RANGES;
            uintptr_t offset = rng() % RANGE_SIZE;
            uintptr_t target = ranges[idx].start + offset;
            size_t result = rt.lookup(addr(target));
            if (result != 0)
                ++found;
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

        std::cout << "Total: " << ns / 1000 << " us" << '\n';
        std::cout << "Per lookup: " << ns / LOOKUPS_PER_TEST << " ns" << '\n';
        std::cout << "Hit rate: " << (found * 100 / LOOKUPS_PER_TEST) << "%\n" << '\n';
    }

    // ========================================================================
    // Test 4: Random lookup throughput (misses - gap addresses)
    // ========================================================================
    {
        std::cout << "--- Test 4: Random Lookup (Misses) ---" << '\n';

        std::mt19937_64 rng(123);
        size_t misses = 0;
        auto start = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < LOOKUPS_PER_TEST; ++i)
        {
            size_t idx = rng() % NUM_RANGES;
            uintptr_t target = ranges[idx].end + (rng() % GAP);
            size_t result = rt.lookup(addr(target));
            if (result == 0)
                ++misses;
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

        std::cout << "Total: " << ns / 1000 << " us" << '\n';
        std::cout << "Per lookup: " << ns / LOOKUPS_PER_TEST << " ns" << '\n';
        std::cout << "Miss rate: " << (misses * 100 / LOOKUPS_PER_TEST) << "%\n" << '\n';
    }

    // ========================================================================
    // Test 5: Correctness verification under load
    // ========================================================================
    {
        std::cout << "--- Test 5: Correctness Verification ---" << '\n';

        size_t errors = 0;

        for (size_t i = 0; i < NUM_RANGES; ++i)
        {
            size_t result = rt.lookup(addr(ranges[i].start));
            if (result != ranges[i].slab_id)
            {
                ++errors;
                if (errors <= 5)
                {
                    std::cerr << "ERROR: Range " << i << " start 0x" << std::hex << ranges[i].start << std::dec << " expected slab_id "
                              << ranges[i].slab_id << " got " << result << '\n';
                }
            }

            result = rt.lookup(addr(ranges[i].start + RANGE_SIZE / 2));
            if (result != ranges[i].slab_id)
            {
                ++errors;
            }

            result = rt.lookup(addr(ranges[i].end));
            if (result != 0)
            {
                ++errors;
                if (errors <= 5)
                {
                    std::cerr << "ERROR: Range " << i << " end 0x" << std::hex << ranges[i].end << std::dec << " should be 0, got " << result << '\n';
                }
            }
        }

        if (errors == 0)
            std::cout << "All " << NUM_RANGES * 3 << " checks passed" << '\n';
        else
            std::cerr << errors << " errors found!" << '\n';

        std::cout << '\n';
    }

    // ========================================================================
    // Test 6: Real heap allocation ranges
    // ========================================================================
    {
        std::cout << "--- Test 6: Real Heap Allocation Ranges ---" << '\n';

        const size_t NUM_ALLOCS = 500;
        const size_t ALLOC_SIZE = 4096;

        AL::radix_tree heap_rt;
        std::vector<void*> blocks;
        blocks.reserve(NUM_ALLOCS);

        for (size_t i = 0; i < NUM_ALLOCS; ++i)
        {
            void* block = std::malloc(ALLOC_SIZE);
            if (!block)
            {
                std::cerr << "ERROR: malloc failed at " << i << '\n';
                for (void* b : blocks)
                    std::free(b);
                return 1;
            }
            blocks.push_back(block);
            uintptr_t b = reinterpret_cast<uintptr_t>(block);
            heap_rt.insert(block, addr(b + ALLOC_SIZE), i + 1);
        }

        size_t errors = 0;
        auto start = std::chrono::high_resolution_clock::now();

        for (size_t rep = 0; rep < 1000; ++rep)
        {
            for (size_t i = 0; i < NUM_ALLOCS; ++i)
            {
                uintptr_t b = reinterpret_cast<uintptr_t>(blocks[i]);
                size_t result = heap_rt.lookup(blocks[i]);
                if (result != i + 1)
                    ++errors;

                result = heap_rt.lookup(addr(b + ALLOC_SIZE / 2));
                if (result != i + 1)
                    ++errors;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

        std::cout << "Lookups: " << NUM_ALLOCS * 2 * 1000 << '\n';
        std::cout << "Total: " << ns / 1000 << " us" << '\n';
        std::cout << "Per lookup: " << ns / (NUM_ALLOCS * 2 * 1000) << " ns" << '\n';

        if (errors == 0)
            std::cout << "All checks passed" << '\n';
        else
            std::cerr << errors << " errors found!" << '\n';

        for (void* b : blocks)
            std::free(b);

        std::cout << '\n';
    }

    // ========================================================================
    // Test 7: Lookup latency consistency (tail latency)
    // ========================================================================
    {
        std::cout << "--- Test 7: Lookup Latency Distribution ---" << '\n';

        const size_t SAMPLES = 1000000;
        std::vector<long long> latencies;
        latencies.reserve(SAMPLES);

        std::mt19937_64 rng(999);

        for (size_t i = 0; i < SAMPLES; ++i)
        {
            size_t idx = rng() % NUM_RANGES;
            uintptr_t target = ranges[idx].start + (rng() % RANGE_SIZE);

            auto start = std::chrono::high_resolution_clock::now();
            volatile size_t result = rt.lookup(addr(target));
            (void)result;
            auto end = std::chrono::high_resolution_clock::now();

            latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        }

        std::sort(latencies.begin(), latencies.end());

        std::cout << "Min:    " << latencies[0] << " ns" << '\n';
        std::cout << "p50:    " << latencies[SAMPLES / 2] << " ns" << '\n';
        std::cout << "p90:    " << latencies[SAMPLES * 90 / 100] << " ns" << '\n';
        std::cout << "p99:    " << latencies[SAMPLES * 99 / 100] << " ns" << '\n';
        std::cout << "p99.9:  " << latencies[SAMPLES * 999 / 1000] << " ns" << '\n';
        std::cout << "Max:    " << latencies[SAMPLES - 1] << " ns" << '\n';

        std::cout << '\n';
    }

    std::cout << "=== All stress tests complete ===" << '\n';
    return 0;
}
