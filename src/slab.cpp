#include "slab.h"
#include "pool.h"
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <strings.h>

namespace AL
{
slab::slab(double scale)
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
{}

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

    if (index < 4)
    {
        // hot size classes
        // should batch
        thread_local_cache& cache = index_to_cache(index);
        if (auto elem = cache.try_pop())
        {
            // cache hit
            return elem;
        }
        else
        {
            // cache miss
            size_t num_allocated = pool.alloc_batched_internal(cache.object_count / 2, cache.objects.data());
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
    if (index < 4)
    {
        // hot size classes
        // should batch
        thread_local_cache& cache = index_to_cache(index);
        if (cache.is_full())
        {
            auto num = cache.object_count / 2;
            pool.free_batched_internal(num, cache.objects.data() + num);
            cache.current = num;
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
    for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i)
        total += shared_pools[i].get_free_space();
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

} // namespace AL
