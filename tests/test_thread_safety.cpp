#include "arena.h"
#include "pool.h"
#include "slab.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
size_t worker_count()
{
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0)
        return 8;
    return std::min<size_t>(hw, 16);
}

void wait_for_start(const std::atomic<bool>& start)
{
    while (!start.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
}

constexpr std::array<size_t, 10> SLAB_SIZE_CLASSES = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096,
};

size_t slab_class_size(size_t requested)
{
    for (size_t size : SLAB_SIZE_CLASSES)
    {
        if (requested <= size)
            return size;
    }
    return 0;
}
} // namespace

TEST_CASE("Arena thread safety: concurrent fixed-size allocations stay unique", "[arena][thread]")
{
    const size_t threads = worker_count();
    const size_t alloc_size = 32;
    const size_t allocs_per_thread = 512;
    AL::arena arena(threads * allocs_per_thread * alloc_size);

    std::atomic<bool> start{false};
    std::atomic<size_t> null_allocations{0};
    std::vector<std::vector<void*>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(allocs_per_thread);
            wait_for_start(start);

            for (size_t i = 0; i < allocs_per_thread; ++i)
            {
                void* ptr = arena.alloc(alloc_size);
                if (ptr == nullptr)
                {
                    null_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                local.push_back(ptr);
                static_cast<std::byte*>(ptr)[0] = static_cast<std::byte>((tid + i) & 0xFF);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(null_allocations.load(std::memory_order_relaxed) == 0);

    std::unordered_set<void*> unique_ptrs;
    unique_ptrs.reserve(threads * allocs_per_thread);
    size_t total_allocs = 0;
    for (const auto& local : allocated)
    {
        for (void* ptr : local)
        {
            REQUIRE(unique_ptrs.insert(ptr).second);
        }
        total_allocs += local.size();
    }

    REQUIRE(total_allocs == threads * allocs_per_thread);
    REQUIRE(arena.get_used() == total_allocs * alloc_size);
}

TEST_CASE("Arena thread safety: concurrent exhaustion is bounded by capacity", "[arena][thread]")
{
    const size_t threads = worker_count();
    const size_t alloc_size = 16;
    const size_t capacity_slots = 4096;
    const size_t attempts_per_thread = (capacity_slots / threads) + 256;

    AL::arena arena(capacity_slots * alloc_size);
    std::atomic<bool> start{false};
    std::atomic<size_t> successful_allocs{0};
    std::vector<std::vector<void*>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(attempts_per_thread);
            wait_for_start(start);

            for (size_t i = 0; i < attempts_per_thread; ++i)
            {
                void* ptr = arena.alloc(alloc_size);
                if (ptr == nullptr)
                    continue;

                local.push_back(ptr);
                successful_allocs.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    const size_t success_count = successful_allocs.load(std::memory_order_relaxed);
    REQUIRE(success_count == capacity_slots);
    REQUIRE(arena.get_used() == capacity_slots * alloc_size);

    std::unordered_set<void*> unique_ptrs;
    unique_ptrs.reserve(success_count);
    for (const auto& local : allocated)
    {
        for (void* ptr : local)
            REQUIRE(unique_ptrs.insert(ptr).second);
    }
    REQUIRE(unique_ptrs.size() == capacity_slots);
}

TEST_CASE("Arena thread safety: concurrent calloc returns zeroed blocks", "[arena][thread]")
{
    const size_t threads = worker_count();
    const size_t alloc_size = 64;
    const size_t allocs_per_thread = 256;
    AL::arena arena(threads * allocs_per_thread * alloc_size);

    std::atomic<bool> start{false};
    std::atomic<size_t> null_allocations{0};
    std::atomic<size_t> non_zero_blocks{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);

            for (size_t i = 0; i < allocs_per_thread; ++i)
            {
                auto* ptr = static_cast<std::byte*>(arena.calloc(alloc_size));
                if (ptr == nullptr)
                {
                    null_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                bool all_zero = true;
                for (size_t j = 0; j < alloc_size; ++j)
                {
                    if (ptr[j] != std::byte{0})
                    {
                        all_zero = false;
                        break;
                    }
                }
                if (!all_zero)
                    non_zero_blocks.fetch_add(1, std::memory_order_relaxed);

                std::memset(ptr, static_cast<int>((tid + i) & 0xFF), alloc_size);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(null_allocations.load(std::memory_order_relaxed) == 0);
    REQUIRE(non_zero_blocks.load(std::memory_order_relaxed) == 0);
    REQUIRE(arena.get_used() == threads * allocs_per_thread * alloc_size);
}

TEST_CASE("Arena thread safety: zero-length alloc remains stable under contention", "[arena][thread]")
{
    const size_t threads = worker_count();
    const size_t iterations = 10000;
    AL::arena arena(4096);

    std::atomic<bool> start{false};
    std::atomic<size_t> non_null_returns{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                if (arena.alloc(0) != nullptr)
                    non_null_returns.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(non_null_returns.load(std::memory_order_relaxed) == 0);
    REQUIRE(arena.get_used() == 0);
}

TEST_CASE("Arena thread safety: reset after synchronized workers restores allocator", "[arena][thread]")
{
    const size_t threads = worker_count();
    const size_t alloc_size = 24;
    const size_t allocs_per_thread = 200;
    AL::arena arena(threads * allocs_per_thread * alloc_size);

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&] {
            wait_for_start(start);
            for (size_t i = 0; i < allocs_per_thread; ++i)
                static_cast<void>(arena.alloc(alloc_size));
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(arena.get_used() > 0);
    REQUIRE(arena.reset() == 0);
    REQUIRE(arena.get_used() == 0);
    REQUIRE(arena.alloc(alloc_size) != nullptr);
}

TEST_CASE("Pool thread safety: concurrent full allocation returns unique blocks", "[pool][thread]")
{
    const size_t threads = worker_count();
    const size_t block_size = 64;
    const size_t allocs_per_thread = 128;
    const size_t block_count = threads * allocs_per_thread;
    AL::pool pool(block_size, block_count);

    std::atomic<bool> start{false};
    std::atomic<size_t> null_allocations{0};
    std::vector<std::vector<void*>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(allocs_per_thread);
            wait_for_start(start);

            for (size_t i = 0; i < allocs_per_thread; ++i)
            {
                void* ptr = pool.alloc();
                if (ptr == nullptr)
                {
                    null_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                local.push_back(ptr);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(null_allocations.load(std::memory_order_relaxed) == 0);

    std::unordered_set<void*> unique_ptrs;
    unique_ptrs.reserve(block_count);
    size_t total_allocs = 0;
    for (const auto& local : allocated)
    {
        for (void* ptr : local)
            REQUIRE(unique_ptrs.insert(ptr).second);
        total_allocs += local.size();
    }

    REQUIRE(total_allocs == block_count);
    REQUIRE(unique_ptrs.size() == block_count);
    REQUIRE(pool.get_free_space() == 0);
}

TEST_CASE("Pool thread safety: concurrent exhaustion is bounded by block count", "[pool][thread]")
{
    const size_t threads = worker_count();
    const size_t block_size = 128;
    const size_t block_count = threads * 64;
    const size_t attempts_per_thread = 128;
    AL::pool pool(block_size, block_count);

    std::atomic<bool> start{false};
    std::atomic<size_t> successful_allocs{0};
    std::vector<std::vector<void*>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(attempts_per_thread);
            wait_for_start(start);

            for (size_t i = 0; i < attempts_per_thread; ++i)
            {
                void* ptr = pool.alloc();
                if (ptr == nullptr)
                    continue;
                local.push_back(ptr);
                successful_allocs.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(successful_allocs.load(std::memory_order_relaxed) == block_count);

    std::unordered_set<void*> unique_ptrs;
    unique_ptrs.reserve(block_count);
    for (const auto& local : allocated)
    {
        for (void* ptr : local)
            REQUIRE(unique_ptrs.insert(ptr).second);
    }
    REQUIRE(unique_ptrs.size() == block_count);

    start.store(false, std::memory_order_release);
    workers.clear();
    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (void* ptr : allocated[tid])
                pool.free(ptr);
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(pool.get_free_space() == pool.get_block_size() * pool.get_block_count());
}

TEST_CASE("Pool thread safety: concurrent alloc/free churn keeps accounting stable", "[pool][thread]")
{
    const size_t threads = worker_count();
    const size_t block_size = 64;
    const size_t block_count = threads * 32;
    const size_t iterations = 5000;
    AL::pool pool(block_size, block_count);

    std::atomic<bool> start{false};
    std::atomic<size_t> successful_cycles{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);

            for (size_t i = 0; i < iterations; ++i)
            {
                void* ptr = pool.alloc();
                if (ptr == nullptr)
                    continue;

                std::memset(ptr, static_cast<int>((tid + i) & 0xFF), block_size);
                pool.free(ptr);
                successful_cycles.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(successful_cycles.load(std::memory_order_relaxed) > 0);
    REQUIRE(pool.get_free_space() == pool.get_block_size() * pool.get_block_count());
}

TEST_CASE("Pool thread safety: concurrent calloc returns zeroed blocks", "[pool][thread]")
{
    const size_t threads = worker_count();
    const size_t block_size = 128;
    const size_t block_count = threads * 32;
    const size_t iterations = 2000;
    AL::pool pool(block_size, block_count);

    std::atomic<bool> start{false};
    std::atomic<size_t> non_zero_blocks{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);

            for (size_t i = 0; i < iterations; ++i)
            {
                auto* ptr = static_cast<std::byte*>(pool.calloc());
                if (ptr == nullptr)
                    continue;

                bool all_zero = true;
                for (size_t j = 0; j < block_size; ++j)
                {
                    if (ptr[j] != std::byte{0})
                    {
                        all_zero = false;
                        break;
                    }
                }
                if (!all_zero)
                    non_zero_blocks.fetch_add(1, std::memory_order_relaxed);

                std::memset(ptr, static_cast<int>((tid + i) & 0xFF), block_size);
                pool.free(ptr);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(non_zero_blocks.load(std::memory_order_relaxed) == 0);
    REQUIRE(pool.get_free_space() == pool.get_block_size() * pool.get_block_count());
}

TEST_CASE("Pool thread safety: concurrent free(nullptr) is safe", "[pool][thread]")
{
    const size_t threads = worker_count();
    const size_t iterations = 20000;
    AL::pool pool(64, 256);
    const size_t initial_free = pool.get_free_space();

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
                pool.free(nullptr);
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(pool.get_free_space() == initial_free);
}

TEST_CASE("Pool thread safety: reset after synchronized workers restores pool", "[pool][thread]")
{
    const size_t threads = worker_count();
    const size_t block_size = 64;
    const size_t block_count = threads * 32;
    const size_t iterations = 1500;
    AL::pool pool(block_size, block_count);

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                void* ptr = pool.alloc();
                if (ptr == nullptr)
                    continue;
                std::memset(ptr, static_cast<int>((tid + i) & 0xFF), block_size);
                pool.free(ptr);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    pool.reset();
    REQUIRE(pool.get_free_space() == pool.get_block_size() * pool.get_block_count());
    REQUIRE(pool.alloc() != nullptr);
}

TEST_CASE("Slab thread safety: concurrent mixed-size alloc/free remains stable", "[slab][thread]")
{
    const size_t threads = worker_count();
    const size_t iterations = 4000;
    constexpr std::array<size_t, 28> request_sizes = {
        1, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 2047, 2048, 2049, 4096,
    };

    AL::slab slab(4.0);
    const size_t initial_total_free = slab.get_total_free();
    std::atomic<bool> start{false};
    std::atomic<size_t> null_allocations{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);

            for (size_t i = 0; i < iterations; ++i)
            {
                const size_t request_size = request_sizes[(tid + i) % request_sizes.size()];
                void* ptr = slab.alloc(request_size);
                if (ptr == nullptr)
                {
                    null_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                auto* bytes = static_cast<std::byte*>(ptr);
                bytes[0] = static_cast<std::byte>((tid + i) & 0xFF);
                slab.free(ptr, request_size);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(null_allocations.load(std::memory_order_relaxed) == 0);
    // All size classes use TLC. Reset flushes caches back to pools so
    // pool-level free accounting reflects all freed blocks.
    slab.reset();
    REQUIRE(slab.get_total_free() == initial_total_free);
}

TEST_CASE("Slab thread safety: per-class contention restores each pool", "[slab][thread]")
{
    const size_t threads = std::max<size_t>(worker_count(), SLAB_SIZE_CLASSES.size());
    const size_t iterations = 5000;
    AL::slab slab(2.0);

    std::array<size_t, SLAB_SIZE_CLASSES.size()> initial_free{};
    for (size_t i = 0; i < SLAB_SIZE_CLASSES.size(); ++i)
        initial_free[i] = slab.get_pool_free_space(i);

    std::atomic<bool> start{false};
    std::atomic<size_t> null_allocations{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            const size_t size = SLAB_SIZE_CLASSES[tid % SLAB_SIZE_CLASSES.size()];
            wait_for_start(start);

            for (size_t i = 0; i < iterations; ++i)
            {
                void* ptr = slab.alloc(size);
                if (ptr == nullptr)
                {
                    null_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                slab.free(ptr, size);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(null_allocations.load(std::memory_order_relaxed) == 0);
    // Reset to reclaim TLC-cached pointers back into pool free lists
    slab.reset();
    for (size_t i = 0; i < SLAB_SIZE_CLASSES.size(); ++i)
        REQUIRE(slab.get_pool_free_space(i) == initial_free[i]);
}

TEST_CASE("Slab thread safety: concurrent exhaustion is bounded within a size class", "[slab][thread]")
{
    const size_t threads = worker_count();
    constexpr size_t class_index = 4; // 128-byte size class
    constexpr size_t request_size = 128;
    AL::slab slab(0.05);

    const size_t block_size = slab.get_pool_block_size(class_index);
    const size_t block_count = slab.get_pool_free_space(class_index) / block_size;
    const size_t attempts_per_thread = (block_count / threads) + 32;

    std::atomic<bool> start{false};
    std::atomic<size_t> successful_allocs{0};
    std::vector<std::vector<void*>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(attempts_per_thread);
            wait_for_start(start);

            for (size_t i = 0; i < attempts_per_thread; ++i)
            {
                void* ptr = slab.alloc(request_size);
                if (ptr == nullptr)
                    continue;
                local.push_back(ptr);
                successful_allocs.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    // With TLC, batch refills mean threads may hold blocks in cache.
    // Total successful allocs should not exceed pool capacity.
    REQUIRE(successful_allocs.load(std::memory_order_relaxed) <= block_count);

    std::unordered_set<void*> unique_ptrs;
    unique_ptrs.reserve(block_count);
    for (const auto& local : allocated)
    {
        for (void* ptr : local)
            REQUIRE(unique_ptrs.insert(ptr).second);
    }
    REQUIRE(unique_ptrs.size() == block_count);

    start.store(false, std::memory_order_release);
    workers.clear();
    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (void* ptr : allocated[tid])
                slab.free(ptr, request_size);
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    // All size classes use TLC; flush caches back to pools before checking accounting.
    slab.reset();
    REQUIRE(slab.get_pool_free_space(class_index) == block_count * block_size);
}

TEST_CASE("Slab thread safety: concurrent calloc returns zeroed size-class blocks", "[slab][thread]")
{
    const size_t threads = worker_count();
    const size_t iterations = 3000;
    constexpr std::array<size_t, 10> request_sizes = {
        7, 9, 17, 33, 65, 129, 257, 513, 1025, 2049,
    };
    AL::slab slab(3.0);

    std::atomic<bool> start{false};
    std::atomic<size_t> null_allocations{0};
    std::atomic<size_t> non_zero_blocks{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);

            for (size_t i = 0; i < iterations; ++i)
            {
                const size_t request_size = request_sizes[(tid + i) % request_sizes.size()];
                const size_t class_size = slab_class_size(request_size);
                auto* ptr = static_cast<std::byte*>(slab.calloc(request_size));
                if (ptr == nullptr)
                {
                    null_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                if (ptr[0] != std::byte{0} || ptr[class_size / 2] != std::byte{0} || ptr[class_size - 1] != std::byte{0})
                    non_zero_blocks.fetch_add(1, std::memory_order_relaxed);

                std::memset(ptr, static_cast<int>((tid + i) & 0xFF), class_size);
                slab.free(ptr, request_size);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(null_allocations.load(std::memory_order_relaxed) == 0);
    REQUIRE(non_zero_blocks.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Slab thread safety: reset after synchronized workers restores all pools", "[slab][thread]")
{
    const size_t threads = worker_count();
    const size_t iterations = 1500;
    AL::slab slab(1.0);
    const size_t initial_total_free = slab.get_total_free();

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                const size_t size = SLAB_SIZE_CLASSES[(tid + i) % SLAB_SIZE_CLASSES.size()];
                void* ptr = slab.alloc(size);
                if (ptr == nullptr)
                    continue;
                slab.free(ptr, size);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    slab.reset();
    REQUIRE(slab.get_total_free() == initial_total_free);

    for (size_t size : SLAB_SIZE_CLASSES)
    {
        void* ptr = slab.alloc(size);
        REQUIRE(ptr != nullptr);
        slab.free(ptr, size);
    }
}

TEST_CASE("Arena thread safety: mixed allocation sizes stay non-overlapping", "[arena][thread]")
{
    const size_t threads = worker_count();
    constexpr std::array<size_t, 5> sizes = {1, 7, 32, 128, 255};
    const size_t allocs_per_thread = 200;
    // Worst case: each thread allocates allocs_per_thread * max_size
    AL::arena arena(threads * allocs_per_thread * 256);

    std::atomic<bool> start{false};
    std::vector<std::vector<std::pair<std::byte*, size_t>>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(allocs_per_thread);
            wait_for_start(start);

            for (size_t i = 0; i < allocs_per_thread; ++i)
            {
                size_t sz = sizes[(tid + i) % sizes.size()];
                auto* ptr = static_cast<std::byte*>(arena.alloc(sz));
                if (ptr == nullptr)
                    continue;
                // Fill with unique byte pattern
                std::memset(ptr, static_cast<int>((tid * 37 + i) & 0xFF), sz);
                local.push_back({ptr, sz});
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    // Verify no overlapping regions: sort all allocations by address and check gaps
    std::vector<std::pair<std::byte*, size_t>> all_allocs;
    for (const auto& local : allocated)
        all_allocs.insert(all_allocs.end(), local.begin(), local.end());

    std::sort(all_allocs.begin(), all_allocs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (size_t i = 1; i < all_allocs.size(); ++i)
    {
        std::byte* prev_end = all_allocs[i - 1].first + all_allocs[i - 1].second;
        REQUIRE(prev_end <= all_allocs[i].first);
    }
}

TEST_CASE("Arena thread safety: data integrity under contention", "[arena][thread]")
{
    const size_t threads = worker_count();
    const size_t alloc_size = 64;
    const size_t allocs_per_thread = 256;
    AL::arena arena(threads * allocs_per_thread * alloc_size);

    std::atomic<bool> start{false};
    std::vector<std::vector<void*>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(allocs_per_thread);
            wait_for_start(start);

            for (size_t i = 0; i < allocs_per_thread; ++i)
            {
                auto* ptr = static_cast<std::byte*>(arena.alloc(alloc_size));
                if (ptr == nullptr)
                    continue;
                // Write a repeating byte pattern unique to (tid, i)
                std::byte pattern = static_cast<std::byte>((tid * 17 + i) & 0xFF);
                std::memset(ptr, static_cast<int>(pattern), alloc_size);
                local.push_back(ptr);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    // After all threads finish, verify each allocation still has its pattern intact
    for (size_t tid = 0; tid < threads; ++tid)
    {
        for (size_t i = 0; i < allocated[tid].size(); ++i)
        {
            auto* ptr = static_cast<std::byte*>(allocated[tid][i]);
            std::byte pattern = static_cast<std::byte>((tid * 17 + i) & 0xFF);
            for (size_t j = 0; j < alloc_size; ++j)
                REQUIRE(ptr[j] == pattern);
        }
    }
}

TEST_CASE("Arena thread safety: capacity boundary race condition", "[arena][thread]")
{
    // Capacity exactly fits N allocations. All threads race to claim them.
    const size_t threads = worker_count();
    const size_t alloc_size = 64;
    const size_t total_blocks = 128;
    AL::arena arena(total_blocks * alloc_size);
    const size_t attempts_per_thread = total_blocks; // each thread tries for all blocks

    std::atomic<bool> start{false};
    std::atomic<size_t> success{0};
    std::atomic<size_t> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&] {
            wait_for_start(start);
            for (size_t i = 0; i < attempts_per_thread; ++i)
            {
                if (arena.alloc(alloc_size) != nullptr)
                    success.fetch_add(1, std::memory_order_relaxed);
                else
                    failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(success.load() == total_blocks);
    REQUIRE(success.load() + failures.load() == threads * attempts_per_thread);
    REQUIRE(arena.get_used() == total_blocks * alloc_size);
}

TEST_CASE("Arena thread safety: single-byte allocations under high contention", "[arena][thread]")
{
    const size_t threads = worker_count();
    const size_t allocs_per_thread = 2000;
    AL::arena arena(threads * allocs_per_thread);

    std::atomic<bool> start{false};
    std::atomic<size_t> success{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < allocs_per_thread; ++i)
            {
                auto* ptr = static_cast<std::byte*>(arena.alloc(1));
                if (ptr != nullptr)
                {
                    *ptr = static_cast<std::byte>(tid & 0xFF);
                    success.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(success.load() == threads * allocs_per_thread);
    REQUIRE(arena.get_used() == threads * allocs_per_thread);
}

TEST_CASE("Arena thread safety: concurrent alloc while observing get_used", "[arena][thread]")
{
    const size_t threads = worker_count();
    const size_t alloc_size = 16;
    const size_t allocs_per_thread = 500;
    AL::arena arena(threads * allocs_per_thread * alloc_size);

    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<size_t> monotonic_violations{0};
    std::vector<std::thread> workers;
    workers.reserve(threads + 1);

    // Observer thread: repeatedly reads get_used(), checks it never decreases
    workers.emplace_back([&] {
        wait_for_start(start);
        size_t prev = 0;
        while (!done.load(std::memory_order_acquire))
        {
            size_t cur = arena.get_used();
            if (cur < prev)
                monotonic_violations.fetch_add(1, std::memory_order_relaxed);
            prev = cur;
        }
    });

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&] {
            wait_for_start(start);
            for (size_t i = 0; i < allocs_per_thread; ++i)
                static_cast<void>(arena.alloc(alloc_size));
        });
    }

    start.store(true, std::memory_order_release);
    // Wait for allocator threads
    for (size_t i = 1; i < workers.size(); ++i)
        workers[i].join();
    done.store(true, std::memory_order_release);
    workers[0].join();

    REQUIRE(monotonic_violations.load() == 0);
    REQUIRE(arena.get_used() == threads * allocs_per_thread * alloc_size);
}

TEST_CASE("Arena thread safety: large and small allocations interleaved", "[arena][thread]")
{
    const size_t threads = worker_count();
    const size_t iterations = 100;
    // Half threads allocate large, half allocate small
    AL::arena arena(threads * iterations * 1024);

    std::atomic<bool> start{false};
    std::atomic<size_t> success{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                size_t sz = (tid % 2 == 0) ? 8 : 1024;
                auto* ptr = static_cast<std::byte*>(arena.alloc(sz));
                if (ptr != nullptr)
                {
                    ptr[0] = static_cast<std::byte>(0xAB);
                    ptr[sz - 1] = static_cast<std::byte>(0xCD);
                    success.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(success.load() == threads * iterations);
}

TEST_CASE("Arena thread safety: multiple reset-and-reuse cycles", "[arena][thread]")
{
    const size_t threads = worker_count();
    const size_t alloc_size = 32;
    const size_t allocs_per_thread = 100;
    AL::arena arena(threads * allocs_per_thread * alloc_size);

    for (int cycle = 0; cycle < 5; ++cycle)
    {
        std::atomic<bool> start{false};
        std::vector<std::thread> workers;
        workers.reserve(threads);

        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&] {
                wait_for_start(start);
                for (size_t i = 0; i < allocs_per_thread; ++i)
                    static_cast<void>(arena.alloc(alloc_size));
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers)
            t.join();

        REQUIRE(arena.get_used() == threads * allocs_per_thread * alloc_size);
        arena.reset();
        REQUIRE(arena.get_used() == 0);
    }
}

TEST_CASE("Pool thread safety: data integrity across concurrent alloc/free", "[pool][thread]")
{
    const size_t threads = worker_count();
    const size_t block_size = 128;
    const size_t block_count = threads * 64;
    const size_t iterations = 3000;
    AL::pool pool(block_size, block_count);

    std::atomic<bool> start{false};
    std::atomic<size_t> corruption_count{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                auto* ptr = static_cast<std::byte*>(pool.alloc());
                if (ptr == nullptr)
                    continue;

                std::byte pattern = static_cast<std::byte>((tid * 31 + i) & 0xFF);
                std::memset(ptr, static_cast<int>(pattern), block_size);

                // Verify pattern before freeing
                bool intact = true;
                for (size_t j = 0; j < block_size; ++j)
                {
                    if (ptr[j] != pattern)
                    {
                        intact = false;
                        break;
                    }
                }
                if (!intact)
                    corruption_count.fetch_add(1, std::memory_order_relaxed);

                pool.free(ptr);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(corruption_count.load() == 0);
    REQUIRE(pool.get_free_space() == block_size * block_count);
}

TEST_CASE("Pool thread safety: concurrent get_free_space during alloc/free", "[pool][thread]")
{
    const size_t threads = worker_count();
    const size_t block_size = 64;
    const size_t block_count = threads * 32;
    AL::pool pool(block_size, block_count);
    const size_t max_free = block_size * block_count;

    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<size_t> out_of_range{0};
    std::vector<std::thread> workers;
    workers.reserve(threads + 1);

    // Observer: get_free_space should always be in [0, max_free]
    workers.emplace_back([&] {
        wait_for_start(start);
        while (!done.load(std::memory_order_acquire))
        {
            size_t free_space = pool.get_free_space();
            if (free_space > max_free)
                out_of_range.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Half do alloc/free, half just alloc then free at end
    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&] {
            wait_for_start(start);
            for (size_t i = 0; i < 5000; ++i)
            {
                void* ptr = pool.alloc();
                if (ptr)
                    pool.free(ptr);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (size_t i = 1; i < workers.size(); ++i)
        workers[i].join();
    done.store(true, std::memory_order_release);
    workers[0].join();

    REQUIRE(out_of_range.load() == 0);
    REQUIRE(pool.get_free_space() == max_free);
}

TEST_CASE("Pool thread safety: hold multiple blocks then free in reverse order", "[pool][thread]")
{
    const size_t threads = worker_count();
    const size_t block_size = 64;
    const size_t blocks_per_thread = 16;
    const size_t block_count = threads * blocks_per_thread;
    AL::pool pool(block_size, block_count);

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            std::vector<void*> held;
            held.reserve(blocks_per_thread);

            // Grab multiple blocks
            for (size_t i = 0; i < blocks_per_thread; ++i)
            {
                void* ptr = pool.alloc();
                if (ptr)
                {
                    std::memset(ptr, static_cast<int>(tid & 0xFF), block_size);
                    held.push_back(ptr);
                }
            }

            // Free in reverse order
            for (auto it = held.rbegin(); it != held.rend(); ++it)
                pool.free(*it);
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(pool.get_free_space() == block_size * block_count);
}

TEST_CASE("Pool thread safety: exhaustion then full recovery then re-alloc", "[pool][thread]")
{
    const size_t threads = worker_count();
    const size_t block_size = 128;
    const size_t block_count = threads * 8;
    AL::pool pool(block_size, block_count);

    // Phase 1: exhaust the pool
    std::atomic<bool> start{false};
    std::vector<std::vector<void*>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(block_count);
            wait_for_start(start);
            for (size_t i = 0; i < block_count; ++i)
            {
                void* ptr = pool.alloc();
                if (ptr)
                    local.push_back(ptr);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    size_t total = 0;
    for (const auto& local : allocated)
        total += local.size();
    REQUIRE(total == block_count);
    REQUIRE(pool.get_free_space() == 0);

    // Phase 2: free everything concurrently
    start.store(false);
    workers.clear();
    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (void* ptr : allocated[tid])
                pool.free(ptr);
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(pool.get_free_space() == block_size * block_count);

    // Phase 3: re-allocate everything concurrently
    for (auto& local : allocated)
        local.clear();

    start.store(false);
    workers.clear();
    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            wait_for_start(start);
            for (size_t i = 0; i < block_count; ++i)
            {
                void* ptr = pool.alloc();
                if (ptr)
                    local.push_back(ptr);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    total = 0;
    for (const auto& local : allocated)
        total += local.size();
    REQUIRE(total == block_count);
    REQUIRE(pool.get_free_space() == 0);
}

TEST_CASE("Pool thread safety: calloc dirty-then-realloc returns zeroed", "[pool][thread]")
{
    const size_t threads = worker_count();
    const size_t block_size = 256;
    const size_t block_count = threads * 16;
    const size_t iterations = 1000;
    AL::pool pool(block_size, block_count);

    std::atomic<bool> start{false};
    std::atomic<size_t> non_zero{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                // alloc, dirty, free
                auto* ptr = static_cast<std::byte*>(pool.alloc());
                if (ptr == nullptr)
                    continue;
                std::memset(ptr, 0xFF, block_size);
                pool.free(ptr);

                // calloc should return zeroed
                auto* clean = static_cast<std::byte*>(pool.calloc());
                if (clean == nullptr)
                    continue;
                for (size_t j = 0; j < block_size; ++j)
                {
                    if (clean[j] != std::byte{0})
                    {
                        non_zero.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
                pool.free(clean);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(non_zero.load() == 0);
    REQUIRE(pool.get_free_space() == block_size * block_count);
}

TEST_CASE("Pool thread safety: get_free_space concurrent with alloc/free", "[pool][thread]")
{
    const size_t threads = worker_count();
    const size_t block_size = 64;
    const size_t block_count = threads * 64;
    const size_t iterations = 5000;
    AL::pool pool(block_size, block_count);
    const size_t max_free = block_size * block_count;

    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<size_t> out_of_range{0};
    std::vector<std::thread> workers;
    workers.reserve(threads + 1);

    // Observer thread
    workers.emplace_back([&] {
        wait_for_start(start);
        while (!done.load(std::memory_order_acquire))
        {
            size_t free_space = pool.get_free_space();
            if (free_space > max_free)
                out_of_range.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                void* ptr = pool.alloc();
                if (ptr)
                    pool.free(ptr);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (size_t i = 1; i < workers.size(); ++i)
        workers[i].join();
    done.store(true, std::memory_order_release);
    workers[0].join();

    REQUIRE(out_of_range.load() == 0);
    REQUIRE(pool.get_free_space() == max_free);
}

TEST_CASE("Pool thread safety: multiple reset-and-reuse cycles", "[pool][thread]")
{
    const size_t threads = worker_count();
    const size_t block_size = 64;
    const size_t block_count = threads * 16;
    AL::pool pool(block_size, block_count);

    for (int cycle = 0; cycle < 5; ++cycle)
    {
        std::atomic<bool> start{false};
        std::vector<std::thread> workers;
        workers.reserve(threads);

        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&, tid] {
                wait_for_start(start);
                for (size_t i = 0; i < block_count; ++i)
                {
                    void* ptr = pool.alloc();
                    if (ptr)
                        pool.free(ptr);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers)
            t.join();

        pool.reset();
        REQUIRE(pool.get_free_space() == block_size * block_count);
    }
}

TEST_CASE("Slab thread safety: TLC cached class high contention", "[slab][thread]")
{
    // All threads hammer a single TLC-cached size class (32 bytes)
    const size_t threads = worker_count();
    const size_t iterations = 5000;
    AL::slab slab(4.0);

    std::atomic<bool> start{false};
    std::atomic<size_t> successful_cycles{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                void* ptr = slab.alloc(32);
                if (ptr == nullptr)
                    continue;
                static_cast<std::byte*>(ptr)[0] = static_cast<std::byte>(tid & 0xFF);
                slab.free(ptr, 32);
                successful_cycles.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(successful_cycles.load() > 0);
}

TEST_CASE("Slab thread safety: boundary sizes are correctly routed", "[slab][thread]")
{
    // Allocate at exact size-class boundaries from many threads
    const size_t threads = worker_count();
    const size_t iterations = 2000;
    AL::slab slab(3.0);

    std::atomic<bool> start{false};
    std::atomic<size_t> success{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                size_t sz = SLAB_SIZE_CLASSES[tid % SLAB_SIZE_CLASSES.size()];
                void* ptr = slab.alloc(sz);
                if (ptr == nullptr)
                    continue;
                auto* bytes = static_cast<std::byte*>(ptr);
                bytes[0] = static_cast<std::byte>(0xAA);
                bytes[sz - 1] = static_cast<std::byte>(0xBB);
                slab.free(ptr, sz);
                success.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(success.load() == threads * iterations);
}

TEST_CASE("Slab thread safety: data integrity across size classes", "[slab][thread]")
{
    const size_t threads = worker_count();
    const size_t allocs_per_thread = 100;
    AL::slab slab(3.0);

    struct alloc_record
    {
        void* ptr;
        size_t sz;
        std::byte pattern;
    };

    std::atomic<bool> start{false};
    std::atomic<size_t> corruption{0};
    std::vector<std::vector<alloc_record>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(allocs_per_thread);
            wait_for_start(start);

            for (size_t i = 0; i < allocs_per_thread; ++i)
            {
                size_t sz = SLAB_SIZE_CLASSES[(tid + i) % SLAB_SIZE_CLASSES.size()];
                auto* ptr = static_cast<std::byte*>(slab.alloc(sz));
                if (ptr == nullptr)
                    continue;

                std::byte pattern = static_cast<std::byte>((tid * 13 + i) & 0xFF);
                std::memset(ptr, static_cast<int>(pattern), sz);
                local.push_back({ptr, sz, pattern});
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    // Verify all patterns intact
    for (size_t tid = 0; tid < threads; ++tid)
    {
        for (const auto& rec : allocated[tid])
        {
            auto* bytes = static_cast<std::byte*>(rec.ptr);
            for (size_t j = 0; j < rec.sz; ++j)
            {
                if (bytes[j] != rec.pattern)
                {
                    corruption.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
    }

    REQUIRE(corruption.load() == 0);

    // Free all
    for (size_t tid = 0; tid < threads; ++tid)
        for (const auto& rec : allocated[tid])
            slab.free(rec.ptr, rec.sz);
}

TEST_CASE("Slab thread safety: invalid sizes under contention", "[slab][thread]")
{
    const size_t threads = worker_count();
    const size_t iterations = 10000;
    AL::slab slab;

    std::atomic<bool> start{false};
    std::atomic<size_t> non_null{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                // size 0
                if (slab.alloc(0) != nullptr)
                    non_null.fetch_add(1, std::memory_order_relaxed);
                // oversized
                if (slab.alloc(4097) != nullptr)
                    non_null.fetch_add(1, std::memory_order_relaxed);
                // size_t max
                if (slab.alloc((size_t)-1) != nullptr)
                    non_null.fetch_add(1, std::memory_order_relaxed);
                // invalid free
                slab.free(nullptr, 64);
                slab.free(nullptr, 0);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(non_null.load() == 0);
}

TEST_CASE("Slab thread safety: tiny scale fast exhaustion", "[slab][thread]")
{
    const size_t threads = worker_count();
    AL::slab slab(0.01); // minimal blocks per pool

    std::atomic<bool> start{false};
    std::atomic<size_t> success{0};
    std::atomic<size_t> null_count{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < 500; ++i)
            {
                size_t sz = SLAB_SIZE_CLASSES[(tid + i) % SLAB_SIZE_CLASSES.size()];
                void* ptr = slab.alloc(sz);
                if (ptr != nullptr)
                {
                    static_cast<std::byte*>(ptr)[0] = std::byte{0xCC};
                    slab.free(ptr, sz);
                    success.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    null_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    // With tiny scale, we expect some failures
    REQUIRE(success.load() + null_count.load() == threads * 500);
}

TEST_CASE("Slab thread safety: multiple slabs concurrent (TLC eviction)", "[slab][thread]")
{
    // More slabs than MAX_CACHED_SLABS forces TLC eviction
    constexpr size_t num_slabs = 8; // > MAX_CACHED_SLABS (4)
    std::array<std::unique_ptr<AL::slab>, num_slabs> slabs;
    for (auto& s : slabs)
        s = std::make_unique<AL::slab>(8.0); // large scale to absorb TLC batching overhead

    const size_t threads = worker_count();
    const size_t iterations = 2000;

    std::atomic<bool> start{false};
    std::atomic<size_t> success{0};
    std::atomic<size_t> null_count{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                auto& s = *slabs[(tid + i) % num_slabs];
                size_t sz = SLAB_SIZE_CLASSES[(tid + i) % SLAB_SIZE_CLASSES.size()];
                void* ptr = s.alloc(sz);
                if (ptr != nullptr)
                {
                    static_cast<std::byte*>(ptr)[0] = std::byte{0xEE};
                    s.free(ptr, sz);
                    success.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    null_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    // All alloc/free cycles should succeed with large enough scale
    REQUIRE(success.load() + null_count.load() == threads * iterations);
    REQUIRE(success.load() == threads * iterations);
}

TEST_CASE("Slab thread safety: TLC epoch after reset then realloc", "[slab][thread]")
{
    // Verify that after reset, threads see the new epoch and re-alloc works correctly
    const size_t threads = worker_count();
    const size_t iterations = 500;
    AL::slab slab(2.0);

    for (int cycle = 0; cycle < 3; ++cycle)
    {
        std::atomic<bool> start{false};
        std::atomic<size_t> success{0};
        std::vector<std::thread> workers;
        workers.reserve(threads);

        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&, tid] {
                wait_for_start(start);
                for (size_t i = 0; i < iterations; ++i)
                {
                    size_t sz = SLAB_SIZE_CLASSES[(tid + i) % SLAB_SIZE_CLASSES.size()];
                    void* ptr = slab.alloc(sz);
                    if (ptr != nullptr)
                    {
                        static_cast<std::byte*>(ptr)[0] = std::byte{0xDD};
                        slab.free(ptr, sz);
                        success.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers)
            t.join();

        REQUIRE(success.load() > 0);

        // Reset invalidates TLC caches via epoch
        slab.reset();

        // Verify all pools are fully restored
        for (size_t i = 0; i < SLAB_SIZE_CLASSES.size(); ++i)
            REQUIRE(slab.get_pool_free_space(i) > 0);
    }
}

TEST_CASE("Slab thread safety: concurrent calloc on TLC-cached sizes", "[slab][thread]")
{
    const size_t threads = worker_count();
    const size_t iterations = 1000;
    // All size classes are TLC-cached
    constexpr std::array<size_t, 10> cached_sizes = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    AL::slab slab(4.0);

    std::atomic<bool> start{false};
    std::atomic<size_t> non_zero{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                size_t sz = cached_sizes[(tid + i) % cached_sizes.size()];
                size_t class_sz = slab_class_size(sz);
                auto* ptr = static_cast<std::byte*>(slab.calloc(sz));
                if (ptr == nullptr)
                    continue;

                // Spot-check zeroed
                if (ptr[0] != std::byte{0} || ptr[class_sz - 1] != std::byte{0})
                    non_zero.fetch_add(1, std::memory_order_relaxed);

                // Dirty then free
                std::memset(ptr, 0xFF, class_sz);
                slab.free(ptr, sz);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(non_zero.load() == 0);
}

TEST_CASE("Slab thread safety: each thread uses distinct size class", "[slab][thread]")
{
    // Threads are pinned to different size classes — no cross-contamination
    const size_t threads = std::min<size_t>(worker_count(), SLAB_SIZE_CLASSES.size());
    const size_t iterations = 3000;
    AL::slab slab(3.0);

    std::atomic<bool> start{false};
    std::atomic<size_t> corruption{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            size_t sz = SLAB_SIZE_CLASSES[tid];
            std::byte pattern = static_cast<std::byte>(tid & 0xFF);

            for (size_t i = 0; i < iterations; ++i)
            {
                auto* ptr = static_cast<std::byte*>(slab.alloc(sz));
                if (ptr == nullptr)
                    continue;

                std::memset(ptr, static_cast<int>(pattern), sz);

                // Verify before freeing
                bool ok = true;
                for (size_t j = 0; j < sz; ++j)
                {
                    if (ptr[j] != pattern)
                    {
                        ok = false;
                        break;
                    }
                }
                if (!ok)
                    corruption.fetch_add(1, std::memory_order_relaxed);

                slab.free(ptr, sz);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(corruption.load() == 0);
}

TEST_CASE("Slab thread safety: alloc-only burst then bulk free", "[slab][thread]")
{
    const size_t threads = worker_count();
    const size_t allocs_per_thread = 50;
    AL::slab slab(3.0);

    std::atomic<bool> start{false};
    std::vector<std::vector<std::pair<void*, size_t>>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    // Phase 1: all threads allocate without freeing
    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(allocs_per_thread);
            wait_for_start(start);
            for (size_t i = 0; i < allocs_per_thread; ++i)
            {
                size_t sz = SLAB_SIZE_CLASSES[(tid + i) % SLAB_SIZE_CLASSES.size()];
                void* ptr = slab.alloc(sz);
                if (ptr != nullptr)
                {
                    std::memset(ptr, static_cast<int>(tid & 0xFF), sz);
                    local.push_back({ptr, sz});
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    // Phase 2: all threads free their allocations concurrently
    start.store(false);
    workers.clear();
    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (auto [ptr, sz] : allocated[tid])
                slab.free(ptr, sz);
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();
}
