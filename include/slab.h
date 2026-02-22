#pragma once

#include "pool.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <utility>

namespace AL
{
struct thread_local_cache
{
    // max capacity — actual batch sizes are tuned per size class
    static constexpr size_t object_count = 128;

    std::array<void*, object_count> objects;
    size_t current = 0;
    size_t batch_size = object_count / 2; // filled by slab on cache init

    [[nodiscard]] void* try_pop()
    {
        if (is_empty())
            return nullptr;

        current--;
        return objects[current];
    }

    void push(void* ptr)
    {
        assert(!is_full() && "Thread local cache is full");

        objects[current] = ptr;
        current++;
    }

    bool is_empty() const
    {
        return current == 0;
    }

    bool is_full() const
    {
        return current == object_count;
    }

    void invalidate()
    {
        current = 0;
    }
};

class slab
{
public:
    // scale is multiplied by the default number of blocks to allocate
    slab(size_t scale = 1.0);
    ~slab();

    slab(const slab&) = delete;
    slab& operator=(const slab&) = delete;
    slab(slab&&) noexcept = delete;
    slab& operator=(slab&&) noexcept = delete;

    // returns: nullptr if failed, else the memory address of the block of memory
    [[nodiscard]] void* alloc(size_t size);

    // returns: nullptr if failed, else the memory address of the block of memory
    [[nodiscard]] void* calloc(size_t size);

    // NOT thread safe
    // returns: -1 if failed
    void reset();

    // returns: -1 if failed
    void free(void* ptr, size_t size);

    size_t get_pool_count() const;
    size_t get_total_capacity() const;
    size_t get_total_free() const;
    size_t get_pool_block_size(size_t index) const;
    size_t get_pool_free_space(size_t index) const;

    // check if pointer belongs to this slab
    bool owns(void* ptr) const;

    static constexpr size_t size_to_index(size_t size)
    {
        for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i)
            if (size <= SIZE_CLASS_CONFIG[i].first)
                return i;
        return static_cast<size_t>(-1);
    }

    static constexpr size_t index_to_size_class(size_t index)
    {
        if (index >= NUM_SIZE_CLASSES)
            return 0;
        return SIZE_CLASS_CONFIG[index].first;
    }

private:
    // compile-time size class configuration
    // <bytes class, number of blocks in class>
    static constexpr std::array<std::pair<size_t, size_t>, 10> SIZE_CLASS_CONFIG = {
        {std::make_pair(8, 512),
         std::make_pair(16, 512),
         std::make_pair(32, 256),
         std::make_pair(64, 256),
         std::make_pair(128, 128),
         std::make_pair(256, 128),
         std::make_pair(512, 64),
         std::make_pair(1024, 64),
         std::make_pair(2048, 32),
         std::make_pair(4096, 32)}
    };
    static_assert(SIZE_CLASS_CONFIG.size() > 0, "Atleast one entry in SIZE_CLASS_CONFIG required.");

    static constexpr size_t MAX_CACHED_SLABS = 4;
    static constexpr size_t NUM_SIZE_CLASSES = std::size(SIZE_CLASS_CONFIG);

    // all size classes are cached via TLC
    static constexpr size_t NUM_CACHED_CLASSES = NUM_SIZE_CLASSES;

    // per class batch sizes. smaller objects get larger batches, larger objects get smaller batches
    static constexpr std::array<size_t, 10> BATCH_SIZES = {
        64, // 8B
        64, // 16B
        32, // 32B
        32, // 64B
        16, // 128B
        16, // 256B
        8,  // 512B
        8,  // 1024B
        4,  // 2048B
        4,  // 4096B
    };
    static_assert(BATCH_SIZES.size() == NUM_SIZE_CLASSES);
    static_assert(NUM_CACHED_CLASSES <= NUM_SIZE_CLASSES,
                  "The number of cached classes must be lower than the amount of size classes available. "
                  "Either decrease the cached classes or increase total number of size classes.");

    struct cache_entry
    {
        size_t epoch;
        slab* owner;
        std::array<thread_local_cache, slab::NUM_CACHED_CLASSES> storage;

        void flush()
        {
            if (!owner)
                return; // should we assert?

            for (size_t i = 0; i < NUM_CACHED_CLASSES; i++)
            {
                // move pointers to appropriate pool from storage?
                auto& cache = storage[i];
                if (cache.is_empty())
                    continue;

                owner->shared_pools[i].free_batched_internal(cache.current, cache.objects.data());
                cache.current = 0;
            }
        }

        void invalidate_all()
        {
            if (!owner)
                return;

            for (size_t i = 0; i < NUM_CACHED_CLASSES; i++)
                storage[i].invalidate();
        }
    };

    thread_local static std::array<cache_entry, MAX_CACHED_SLABS> caches;

    cache_entry* get_cached_slab()
    {
        assert(MAX_CACHED_SLABS != 0 && "Cannot get cached slab. Number of cached slabs is 0");

        size_t current = 0;
        size_t empty_cache_entry_index = -1;
        for (auto& cache : caches)
        {
            if (this == cache.owner)
            {
                // found a cache entry belonging to the thread
                return &cache;
            }
            else if (cache.owner == nullptr)
            {
                // found empty entry
                empty_cache_entry_index = current;
            }

            current++;
        }

        // none of the cache entries belong to this thread but we have an empty slot.
        if (empty_cache_entry_index != (size_t)-1)
        {
            cache_entry& entry = caches[empty_cache_entry_index];
            entry.owner = this;
            entry.epoch = epoch.load(std::memory_order_acquire);
            init_cache_batch_sizes(entry);
            return &entry;
        }

        // none of the cache entries belong to this slab and all entries are full
        // we need to evict the last entry
        cache_entry& entry = caches[caches.max_size() - 1];
        entry.flush();
        entry.owner = this;
        entry.epoch = epoch.load(std::memory_order_acquire);
        init_cache_batch_sizes(entry);
        return &entry;
    }

    static void init_cache_batch_sizes(cache_entry& entry)
    {
        for (size_t i = 0; i < NUM_CACHED_CLASSES; ++i)
            entry.storage[i].batch_size = BATCH_SIZES[i];
    }

    std::atomic<size_t> epoch;
    std::array<pool, NUM_SIZE_CLASSES> shared_pools;
};

} // namespace AL
