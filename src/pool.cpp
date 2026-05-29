#include "pool.h"
#include "platform.h"
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <iostream>

namespace AL
{
pool::pool()
{
    clear();
}

pool::pool(size_t block_size, size_t block_count) : pool()
{
    init(block_size, block_count);
}

pool::pool(pool&& other) noexcept : m_region(other.m_region), m_region_size(other.m_region_size), m_view(std::move(other.m_view))
{
    other.clear();
}

pool& pool::operator=(pool&& other) noexcept
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

void pool::init(size_t block_size, size_t block_count)
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
    size_t region_needed = pool_view::required_region_size(block_size, block_count);
    m_region_size = ((region_needed + page_size - 1) / page_size) * page_size;

    void* ptr = AL::platform_mem::alloc(m_region_size);
    if (ptr == nullptr)
        throw std::bad_alloc();

    m_region = static_cast<std::byte*>(ptr);
    m_view.init_from_region(m_region, block_size, block_count);
}

pool::~pool()
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

void* pool::alloc()
{
    check_asserts();

    void* ptr = m_view.alloc();
    return ptr;
}

size_t pool::alloc_batched_internal(size_t num_objects, void* out[])
{
    if (!out)
        return 0;

    check_asserts();

    size_t n = m_view.alloc_batch(num_objects, out);
    return n;
}

void pool::free_batched_internal(size_t num_objects, void* in[])
{
    if (!in)
        return;

    check_asserts();

    std::span<void*> s(in, num_objects);
    m_view.free_batch(s);
}

void* pool::calloc()
{
    void* ptr = alloc();
    if (ptr != nullptr)
        std::memset(ptr, 0, m_view.block_size());
    return ptr;
}

void pool::reset()
{
    check_asserts();
    m_view.reset();
}

void pool::clear()
{
    m_region = nullptr;
    m_region_size = 0;
    m_view = pool_view{};
}

bool pool::owns(void* ptr) const
{
    return m_view.owns(ptr);
}

void pool::free(void* ptr)
{
    if (ptr == nullptr)
        return;

    check_asserts();
    assert(owns(ptr) && "Pointer does not belong to this pool");

    m_view.free(ptr);
}

size_t pool::get_free_space() const
{
    return m_view.free_count() * m_view.block_size();
}

size_t pool::get_capacity() const
{
    return m_view.capacity();
}

size_t pool::get_block_size() const
{
    return m_view.block_size();
}

size_t pool::get_block_count() const
{
    return m_view.block_count();
}

void pool::check_asserts() const
{
#if PALLOC_DEBUG
    assert(m_view.is_initialized() && "pool not initialized correctly.");
#endif
}

} // namespace AL
