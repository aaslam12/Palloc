#include "bitmap.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
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
    if (hw == 0) return 8;
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

    std::cout << "\n=== Bitmap Stress Test ===\n";
    std::cout << "Threads: " << threads << "\n\n";

    // ========================================================================
    // Test 1: High-contention alloc/free churn
    // Each thread repeatedly alloc_bit / free_bit on a shared bitmap.
    // Verifies lock-free correctness and measures throughput.
    // ========================================================================
    {
        const size_t num_slots        = threads * 65536;
        const size_t iters_per_thread = 5000000;

        std::vector<uint8_t> buf(bitmap<>::required_size(num_slots), 0);
        bitmap<> bm;
        bm.init(buf.data(), num_slots);

        std::atomic<bool>   start{false};
        std::atomic<size_t> successful_cycles{0};
        std::vector<std::thread> workers;
        workers.reserve(threads);

        auto t0 = std::chrono::high_resolution_clock::now();

        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&] {
                wait_for_start(start);
                for (size_t i = 0; i < iters_per_thread; ++i)
                {
                    size_t s = bm.alloc_bit();
                    if (s == static_cast<size_t>(-1)) continue;
                    bm.free_bit(s);
                    successful_cycles.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers) t.join();

        auto t1 = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = t1 - t0;

        if (bm.free_count() != num_slots)
        {
            std::cerr << "ERROR: bitmap not fully free after churn. free_count="
                      << bm.free_count() << " expected=" << num_slots << "\n";
            return 1;
        }

        const size_t cycles = successful_cycles.load(std::memory_order_relaxed);
        std::cout << "--- Test 1: High-contention alloc/free churn ---\n"
                  << "Successful cycles: " << cycles << "\n"
                  << "Elapsed:           " << elapsed.count() << " s\n"
                  << "Ops/sec:           " << (cycles * 2 / elapsed.count()) << "\n"
                  << "[PASSED]\n\n";
    }

    // ========================================================================
    // Test 2: Concurrent full exhaustion — no duplicates, exact count
    // All threads race to alloc_bit until the bitmap is full.
    // ========================================================================
    {
        const size_t num_slots = threads * 262144;

        std::vector<uint8_t> buf(bitmap<>::required_size(num_slots), 0);
        bitmap<> bm;
        bm.init(buf.data(), num_slots);

        std::atomic<bool>   start{false};
        std::atomic<size_t> total_allocs{0};
        std::vector<std::vector<size_t>> per_thread(threads);
        std::vector<std::thread> workers;
        workers.reserve(threads);

        auto t0 = std::chrono::high_resolution_clock::now();

        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&, tid] {
                auto& local = per_thread[tid];
                local.reserve(num_slots / threads + 64);
                wait_for_start(start);
                while (true)
                {
                    size_t s = bm.alloc_bit();
                    if (s == static_cast<size_t>(-1)) break;
                    local.push_back(s);
                    total_allocs.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers) t.join();

        auto t1 = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = t1 - t0;

        if (total_allocs.load() != num_slots)
        {
            std::cerr << "ERROR: exhaustion mismatch. got=" << total_allocs.load()
                      << " expected=" << num_slots << "\n";
            return 1;
        }

        std::unordered_set<size_t> unique;
        unique.reserve(num_slots);
        for (auto& local : per_thread)
            for (size_t s : local)
                if (!unique.insert(s).second)
                {
                    std::cerr << "ERROR: duplicate slot " << s << "\n";
                    return 1;
                }

        if (unique.size() != num_slots)
        {
            std::cerr << "ERROR: unique count mismatch\n";
            return 1;
        }

        std::cout << "--- Test 2: Concurrent full exhaustion ---\n"
                  << "Slots exhausted: " << num_slots << "\n"
                  << "Elapsed:         " << elapsed.count() << " s\n"
                  << "[PASSED]\n\n";
    }

    // ========================================================================
    // Test 3: Batch alloc/free churn
    // Threads repeatedly alloc_bits_batch and free_bits_batch.
    // ========================================================================
    {
        const size_t num_slots        = threads * 65536;
        const size_t batch            = 64;
        const size_t iters_per_thread = 100000;

        std::vector<uint8_t> buf(bitmap<>::required_size(num_slots), 0);
        bitmap<> bm;
        bm.init(buf.data(), num_slots);

        std::atomic<bool>   start{false};
        std::atomic<size_t> successful_batches{0};
        std::vector<std::thread> workers;
        workers.reserve(threads);

        auto t0 = std::chrono::high_resolution_clock::now();

        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&] {
                size_t slots[batch];
                wait_for_start(start);
                for (size_t i = 0; i < iters_per_thread; ++i)
                {
                    size_t n = bm.alloc_bits_batch(batch, slots);
                    if (n == 0) continue;
                    std::sort(slots, slots + n);
                    bm.free_bits_batch(slots, n);
                    successful_batches.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers) t.join();

        auto t1 = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = t1 - t0;

        if (bm.free_count() != num_slots)
        {
            std::cerr << "ERROR: free_count=" << bm.free_count()
                      << " expected=" << num_slots << "\n";
            return 1;
        }

        const size_t batches = successful_batches.load(std::memory_order_relaxed);
        std::cout << "--- Test 3: Batch alloc/free churn ---\n"
                  << "Successful batches: " << batches << "\n"
                  << "Slots processed:    " << batches * batch * 2 << "\n"
                  << "Elapsed:            " << elapsed.count() << " s\n"
                  << "Slots/sec:          " << (batches * batch * 2 / elapsed.count()) << "\n"
                  << "[PASSED]\n\n";
    }

    // ========================================================================
    // Test 4: Limit-constrained concurrent alloc
    // Threads race alloc_bit(limit) where limit < num_slots to exercise
    // the tail-mask and hint logic in the limited alloc path.
    // ========================================================================
    {
        const size_t num_slots = 65536;
        const size_t limit     = num_slots / 2;

        std::vector<uint8_t> buf(bitmap<>::required_size(num_slots), 0);
        bitmap<> bm;
        bm.init(buf.data(), num_slots);

        std::atomic<bool>   start{false};
        std::atomic<size_t> total_allocs{0};
        std::vector<std::thread> workers;
        workers.reserve(threads);

        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&] {
                wait_for_start(start);
                while (true)
                {
                    size_t s = bm.alloc_bit(limit);
                    if (s == static_cast<size_t>(-1)) break;
                    if (s >= limit)
                    {
                        std::cerr << "ERROR: slot " << s << " exceeds limit " << limit << "\n";
                        std::exit(1);
                    }
                    total_allocs.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers) t.join();

        if (total_allocs.load() != limit)
        {
            std::cerr << "ERROR: limit alloc count=" << total_allocs.load()
                      << " expected=" << limit << "\n";
            return 1;
        }

        // slots beyond limit untouched
        if (bm.free_count() != num_slots - limit)
        {
            std::cerr << "ERROR: free_count=" << bm.free_count()
                      << " expected=" << (num_slots - limit) << "\n";
            return 1;
        }

        std::cout << "--- Test 4: Limit-constrained concurrent alloc ---\n"
                  << "Slots within limit: " << total_allocs.load() << " / " << limit << "\n"
                  << "Slots untouched:    " << bm.free_count() << "\n"
                  << "[PASSED]\n\n";
    }

    std::cout << "========================================\n"
              << "[PASSED] All bitmap stress tests passed!\n"
              << "========================================\n\n";
    return 0;
}
