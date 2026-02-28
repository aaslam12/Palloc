#include "slab.h"
#include "pool.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <strings.h>

namespace AL
{
// to satisfy the linker
thread_local std::array<slab::cache_entry, slab::MAX_CACHED_SLABS> slab::caches = {};
std::atomic<size_t> slab::next_slab_id{0};

slab::slab(size_t scale) : epoch(0), slab_id(next_slab_id.fetch_add(1, std::memory_order_relaxed))
{
    for (size_t i = 0; i < shared_pools.size(); i++)
    {
        size_t count = static_cast<size_t>(std::ceil(SIZE_CLASS_CONFIG[i].second * scale));
        if (count < 1)
            count = 1;
        shared_pools[i].init(SIZE_CLASS_CONFIG[i].first, count);
    }
}

slab::~slab()
{
    // Check preferred slot first (O(1) fast path)
    const size_t preferred = slab_id % MAX_CACHED_SLABS;
    if (caches[preferred].owner == this)
    {
        caches[preferred].invalidate_all();
        caches[preferred].owner = nullptr;
        return;
    }
    // Fallback scan: entry may have been displaced to another slot
    for (size_t i = 0; i < MAX_CACHED_SLABS; ++i)
    {
        if (i == preferred)
            continue;
        if (caches[i].owner == this)
        {
            caches[i].invalidate_all();
            caches[i].owner = nullptr;
            return;
        }
    }
}

void* slab::alloc(size_t size)
{
    if (size == 0 || size == (size_t)-1)
        return nullptr;
    if (SIZE_CLASS_CONFIG[NUM_SIZE_CLASSES - 1].first < size)
        return nullptr;

    size_t index = size_to_index(size);
    if (index == (size_t)-1)
    {
        return nullptr;
    }

    pool& pool = shared_pools[index];

    if (index < NUM_CACHED_CLASSES)
    {
        // hot size classes
        // should batch
        auto cached_entry = get_cached_slab();
        thread_local_cache& cache = cached_entry->storage[index];
        size_t current_epoch = epoch.load(std::memory_order_acquire);
        if (cached_entry->epoch != current_epoch)
        {
            cached_entry->invalidate_all();
            cached_entry->epoch = current_epoch;
        }

        if (auto elem = cache.try_pop())
        {
            // cache hit
            return elem;
        }
        else
        {
            // cache miss
            size_t num_allocated = pool.alloc_batched_internal(cache.batch_size, cache.objects.data());
            cache.current = num_allocated;

            return cache.try_pop();
        }
    }
    else
    {
        return pool.alloc();
    }
}

void* slab::calloc(size_t size)
{
    void* ptr = alloc(size);

    if (ptr != nullptr)
    {
        // should instead refactor to call pool calloc()
        size_t actual_size = SIZE_CLASS_CONFIG[size_to_index(size)].first;
        std::memset(ptr, 0, actual_size); // zeroes out the entire block, just need the number of bytes, the user requested
    }

    return ptr;
}

void slab::reset()
{
    for (auto& pool : shared_pools)
    {
        pool.reset();
    }
    epoch.fetch_add(1, std::memory_order_release);
}

void slab::free(void* ptr, size_t size)
{
    if (size == 0 || size == (size_t)-1)
        return;
    if (SIZE_CLASS_CONFIG[NUM_SIZE_CLASSES - 1].first < size)
        return;

    size_t index = size_to_index(size);
    if (index == (size_t)-1)
    {
        return;
    }

    pool& pool = shared_pools[index];
    if (index < NUM_CACHED_CLASSES)
    {
        // hot size classes
        // should batch
        auto cached_entry = get_cached_slab();
        thread_local_cache& cache = cached_entry->storage[index];
        size_t current_epoch = epoch.load(std::memory_order_acquire);
        if (cached_entry->epoch != current_epoch)
        {
            cached_entry->invalidate_all();
            cached_entry->epoch = current_epoch;
        }

        if (cache.is_full())
        {
            pool.free_batched_internal(cache.batch_size, cache.objects.data() + (cache.current - cache.batch_size));
            cache.current -= cache.batch_size;
        }

        cache.push(ptr);
    }
    else
    {
        // cache miss
        shared_pools[index].free(ptr);
    }
}

size_t slab::get_pool_count() const
{
    return std::size(shared_pools);
}

size_t slab::get_total_capacity() const
{
    size_t total = 0;
    for (const auto& pool : shared_pools)
    {
        total += pool.get_capacity();
    }
    return total;
}

size_t slab::get_total_free() const
{
    size_t total = 0;
    for (const auto& pool : shared_pools)
        total += pool.get_free_space();
    return total;
}

size_t slab::get_pool_block_size(size_t index) const
{
    if (index >= NUM_SIZE_CLASSES)
        return 0;
    return shared_pools[index].get_block_size();
}

size_t slab::get_pool_free_space(size_t index) const
{
    if (index >= NUM_SIZE_CLASSES)
        return 0;
    return shared_pools[index].get_free_space();
}

bool slab::owns(void* ptr) const
{
    for (const auto& pool : shared_pools)
        if (pool.owns(ptr))
            return true;

    return false;
}

} // namespace AL
