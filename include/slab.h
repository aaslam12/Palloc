#pragma once

#include "pool.h"
#include <array>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <utility>

namespace AL
{
struct thread_local_cache
{
    // configurable value to control cache object count in bytes
    static constexpr size_t object_count = 128;

    std::array<void*, object_count> objects;
    size_t current = 0;

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
};

class slab
{
public:
    // scale is multiplied by the default number of blocks to allocate
    slab(double scale = 1.0);
    ~slab();

    slab(const slab&) = delete;
    slab& operator=(const slab&) = delete;
    slab(slab&&) noexcept = default;
    slab& operator=(slab&&) noexcept = default;

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

private:
    // compile-time size class configuration
    // <bytes class, number of blocks in class>
    static constexpr std::pair<size_t, size_t> SIZE_CLASS_CONFIG[] = {
        {   8, 512},
        {  16, 512},
        {  32, 256},
        {  64, 256},
        { 128, 128},
        { 256, 128},
        { 512,  64},
        {1024,  64},
        {2048,  32},
        {4096,  32},
    };

    static constexpr size_t NUM_SIZE_CLASSES = std::size(SIZE_CLASS_CONFIG);

    static constexpr size_t size_to_index(size_t size)
    {
        for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i)
            if (size <= SIZE_CLASS_CONFIG[i].first)
                return i;

        return -1;
    }

    static constexpr size_t index_to_size_class(size_t index)
    {
        return SIZE_CLASS_CONFIG[index].first;
    }

    static constexpr size_t index_to_block_count(size_t index)
    {
        return SIZE_CLASS_CONFIG[index].second;
    }

    static constexpr thread_local_cache& index_to_cache(size_t index)
    {
        if (index == 0)
        {
            return cache_8B;
        }
        else if (index == 1)
        {
            return cache_16B;
        }
        else if (index == 2)
        {
            return cache_32B;
        }
        else
        {
            return cache_64B;
        }
    }

    thread_local static thread_local_cache cache_8B;
    thread_local static thread_local_cache cache_16B;
    thread_local static thread_local_cache cache_32B;
    thread_local static thread_local_cache cache_64B;

    std::array<pool, NUM_SIZE_CLASSES> shared_pools;
};
} // namespace AL
