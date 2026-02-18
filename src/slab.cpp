#include "slab.h"
#include "pool.h"
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>

namespace AL
{
slab::slab(double scale)
{
    for (size_t i = 0; i < pools.size(); i++)
    {
        size_t count = static_cast<size_t>(std::ceil(SIZE_CLASS_CONFIG[i].second * scale));
        if (count < 1)
            count = 1;
        pools[i].init(SIZE_CLASS_CONFIG[i].first, count);
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

    return pools[index].alloc();
}

void* slab::calloc(size_t size)
{
    void* ptr = alloc(size);

    if (ptr != nullptr)
    {
        size_t actual_size = SIZE_CLASS_CONFIG[size_to_index(size)].first;
        std::memset(ptr, 0, actual_size); // zeroes out the entire block, just need the number of bytes, the user requested
    }

    return ptr;
}

void slab::reset()
{
    for (auto& pool : pools)
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

    pools[index].free(ptr);
}

size_t slab::get_pool_count() const
{
    return std::size(pools);
}

size_t slab::get_total_capacity() const
{
    size_t total = 0;
    for (const auto& pool : pools)
    {
        total += pool.get_capacity();
    }
    return total;
}

size_t slab::get_total_free() const
{
    size_t total = 0;
    for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i)
        total += pools[i].get_free_space();
    return total;
}

size_t slab::get_pool_block_size(size_t index) const
{
    if (index >= NUM_SIZE_CLASSES)
        return 0;
    return pools[index].get_block_size();
}

size_t slab::get_pool_free_space(size_t index) const
{
    if (index >= NUM_SIZE_CLASSES)
        return 0;
    return pools[index].get_free_space();
}

} // namespace AL
