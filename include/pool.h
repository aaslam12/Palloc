#pragma once

#include "palloc_atomic.h"
#include "platform.h"
#include "pool_view.h"
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <new>
#include <span>

namespace AL
{

template<bool Tthreaded = PALLOC_THREADED_DEFAULT>
class alignas(64) pool
{
public:
    pool();
    pool(size_t block_size, size_t block_count);
    ~pool();

    pool(const pool&) = delete;
    pool& operator=(const pool&) = delete;
    pool(pool&&) noexcept;
    pool& operator=(pool&&) noexcept;

    void init(size_t block_size, size_t block_count);

    // allocates a block of memory from the pool
    // returns properly aligned memory
    // thread-safe
    // returns: nullptr if failed, else the memory address of the block of memory
    [[nodiscard]] void* alloc();

    // allocates a block of memory from the pool
    // also zeroes out the memory returned
    // returns properly aligned memory
    // thread-safe
    // returns: nullptr if failed, else the memory address of the block of memory
    [[nodiscard]] void* calloc();

    // frees the entire pool but keeps it alive to reuse
    // thread-safe
    void reset();

    // frees the block
    // thread-safe
    void free(void* ptr);

    // already thread safe (atomic)
    // returns number of free bytes
    size_t get_free_space() const;

    // gets the total amount of bytes that can be used by the pool
    size_t get_capacity() const;

    size_t get_block_size() const;
    size_t get_block_count() const;
    void clear();
    bool owns(void* ptr) const;

    // batched operations
    size_t alloc_batched_internal(size_t num_objects, void* out[]);
    void free_batched_internal(size_t num_objects, void* in[]);

    std::byte* get_memory_start() const
    {
        return m_view.memory_start();
    }
    std::byte* get_memory_end() const
    {
        return m_view.memory_end();
    }

private:
    std::byte* m_region = nullptr; // owned mmap'd memory
    size_t m_region_size = 0;      // total mmap'd size
    pool_view<Tthreaded> m_view;   // non owning pool_view that is given memory by this class
    void check_asserts() const;
};

template<bool Tthreaded>
pool<Tthreaded>::pool()
{
    clear();
}

template<bool Tthreaded>
pool<Tthreaded>::pool(size_t block_size, size_t block_count) : pool()
{
    init(block_size, block_count);
}

template<bool Tthreaded>
pool<Tthreaded>::pool(pool&& other) noexcept : m_region(other.m_region), m_region_size(other.m_region_size), m_view(std::move(other.m_view))
{
    other.clear();
}

template<bool Tthreaded>
pool<Tthreaded>& pool<Tthreaded>::operator=(pool&& other) noexcept
{
    if (this == &other)
        return *this;
    if (m_region != nullptr)
        AL::platform_mem::free(m_region, m_region_size);
    m_region = other.m_region;
    m_region_size = other.m_region_size;
    m_view = std::move(other.m_view);
    other.clear();
    return *this;
}

template<bool Tthreaded>
void pool<Tthreaded>::init(size_t block_size, size_t block_count)
{
    assert(m_region == nullptr && "pool likely already initialized correctly.");
    if (block_size < sizeof(void*))
    {
#if PALLOC_DEBUG
        std::cerr << "WARNING: Pool block size " << block_size << " is too small. "
                  << "Rounded up to " << sizeof(void*) << " bytes.\n";
#endif
        block_size = sizeof(void*);
    }
    block_size = std::bit_ceil(block_size);
    size_t page_size = AL::platform_mem::page_size();
    size_t region_needed = pool_view<Tthreaded>::required_region_size(block_size, block_count);
    m_region_size = ((region_needed + page_size - 1) / page_size) * page_size;
    void* ptr = AL::platform_mem::alloc(m_region_size);
    if (ptr == nullptr)
        throw std::bad_alloc();
    m_region = static_cast<std::byte*>(ptr);
    m_view.init_from_region(m_region, block_size, block_count);
}

template<bool Tthreaded>
pool<Tthreaded>::~pool()
{
    if (m_region == nullptr)
        return;
    bool freed = AL::platform_mem::free(m_region, m_region_size);
#if PALLOC_DEBUG
    if (!freed)
        std::cerr << "WARNING: munmap failed in pool destructor\n";
#endif
    m_region = nullptr;
}

template<bool Tthreaded>
void* pool<Tthreaded>::alloc()
{
    check_asserts();
    return m_view.alloc();
}

template<bool Tthreaded>
void* pool<Tthreaded>::calloc()
{
    check_asserts();
    return m_view.calloc();
}

template<bool Tthreaded>
void pool<Tthreaded>::reset()
{
    check_asserts();
    m_view.reset();
}

template<bool Tthreaded>
void pool<Tthreaded>::free(void* ptr)
{
    if (ptr == nullptr)
        return;
    check_asserts();
    assert(owns(ptr) && "Pointer does not belong to this pool");
    m_view.free(ptr);
}

template<bool Tthreaded>
size_t pool<Tthreaded>::alloc_batched_internal(size_t num_objects, void* out[])
{
    if (!out)
        return 0;
    check_asserts();
    return m_view.alloc_batch(num_objects, out);
}

template<bool Tthreaded>
void pool<Tthreaded>::free_batched_internal(size_t num_objects, void* in[])
{
    if (!in)
        return;
    check_asserts();
    m_view.free_batch(std::span<void*>(in, num_objects));
}

template<bool Tthreaded>
size_t pool<Tthreaded>::get_free_space() const
{
    return m_view.free_count() * m_view.block_size();
}

template<bool Tthreaded>
size_t pool<Tthreaded>::get_capacity() const
{
    return m_view.capacity();
}

template<bool Tthreaded>
size_t pool<Tthreaded>::get_block_size() const
{
    return m_view.block_size();
}

template<bool Tthreaded>
size_t pool<Tthreaded>::get_block_count() const
{
    return m_view.block_count();
}

template<bool Tthreaded>
bool pool<Tthreaded>::owns(void* ptr) const
{
    return m_view.owns(ptr);
}

template<bool Tthreaded>
void pool<Tthreaded>::clear()
{
    m_region = nullptr;
    m_region_size = 0;
    m_view = pool_view<Tthreaded>{};
}

template<bool Tthreaded>
void pool<Tthreaded>::check_asserts() const
{
#if PALLOC_DEBUG
    assert(m_view.is_initialized() && "pool not initialized correctly.");
#endif
}

} // namespace AL
