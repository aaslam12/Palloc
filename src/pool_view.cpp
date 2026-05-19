#include "pool_view.h"
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>

namespace AL
{

pool_view::pool_view(pool_view&& other) noexcept
    : m_memory(other.m_memory)
    , m_block_size(other.m_block_size)
    , m_block_count(other.m_block_count)
    , m_block_shift(other.m_block_shift)
    , m_bitmap(std::move(other.m_bitmap))
{
    other.m_memory = nullptr;
}

pool_view& pool_view::operator=(pool_view&& other) noexcept
{
    if (this == &other) return *this;
    m_memory      = other.m_memory;
    m_block_size  = other.m_block_size;
    m_block_count = other.m_block_count;
    m_block_shift = other.m_block_shift;
    m_bitmap      = std::move(other.m_bitmap);
    other.m_memory = nullptr;
    return *this;
}

size_t pool_view::required_region_size(size_t block_size, size_t block_count) noexcept
{
    size_t bitmap_bytes = bitmap::required_size(block_count);
    size_t aligned_offset = ((bitmap_bytes + block_size - 1) / block_size) * block_size;
    return aligned_offset + block_size * block_count;
}

void pool_view::init_from_region(void* base, size_t block_size, size_t block_count) noexcept
{
    assert(base != nullptr);
    assert(block_size > 0 && std::has_single_bit(block_size));
    assert(block_size >= sizeof(void*));
    assert(block_count > 0);
    assert((reinterpret_cast<uintptr_t>(base) % block_size) == 0);

    m_block_size  = block_size;
    m_block_count = block_count;
    m_block_shift = static_cast<size_t>(std::countr_zero(block_size));

    // bitmap lives at the start of the region
    std::memset(base, 0, bitmap::required_size(block_count));
    m_bitmap.init(base, block_count);

    // payload starts after the bitmap, aligned to block_size
    size_t bitmap_bytes = bitmap::required_size(block_count);
    void* payload_ptr = static_cast<std::byte*>(base) + bitmap_bytes;
    size_t remaining = required_region_size(block_size, block_count) - bitmap_bytes;
    void* aligned = std::align(block_size, block_size * block_count, payload_ptr, remaining);
    assert(aligned != nullptr);
    m_memory = static_cast<std::byte*>(aligned);
}

void pool_view::init_from_region(void* base, void* bitmap_mem, size_t block_size, size_t block_count) noexcept
{
    assert(base != nullptr);
    assert(bitmap_mem != nullptr);
    assert(block_size > 0 && std::has_single_bit(block_size));
    assert(block_size >= sizeof(void*));
    assert(block_count > 0);
    assert((reinterpret_cast<uintptr_t>(base) % block_size) == 0);

    m_block_size  = block_size;
    m_block_count = block_count;
    m_block_shift = static_cast<size_t>(std::countr_zero(block_size));

    // bitmap_mem is pre-zeroed by the caller
    m_bitmap.init(bitmap_mem, block_count);
    m_memory = static_cast<std::byte*>(base);
}

void* pool_view::alloc() noexcept
{
    size_t slot = m_bitmap.alloc_bit();
    if (slot == static_cast<size_t>(-1))
        return nullptr;
    return m_memory + (slot << m_block_shift);
}

size_t pool_view::alloc_batch(size_t count, void* out[]) noexcept
{
    // Use a local slot buffer then convert to pointers
    // Stack-allocate up to 128 slots (max TLC batch size)
    size_t slots[128];
    size_t n = count > 128 ? 128 : count;
    size_t found = m_bitmap.alloc_bits_batch(n, slots);
    for (size_t i = 0; i < found; ++i)
        out[i] = m_memory + (slots[i] << m_block_shift);
    return found;
}

void* pool_view::calloc() noexcept
{
    void* ptr = alloc();
    if (ptr != nullptr)
        std::memset(ptr, 0, m_block_size);
    return ptr;
}

void pool_view::free(void* ptr) noexcept
{
    if (ptr == nullptr) return;
    assert(owns(ptr));
    size_t offset = static_cast<size_t>(static_cast<std::byte*>(ptr) - m_memory);
    size_t slot = offset >> m_block_shift;
    m_bitmap.free_bit(slot);
}

void pool_view::free_batch(std::span<void*> ptrs) noexcept
{
    size_t slots[128];
    size_t count = 0;
    for (void* ptr : ptrs)
    {
        if (ptr == nullptr) continue;
        assert(owns(ptr));
        size_t offset = static_cast<size_t>(static_cast<std::byte*>(ptr) - m_memory);
        slots[count++] = offset >> m_block_shift;
        if (count == 128)
        {
            m_bitmap.free_bits_batch(slots, count);
            count = 0;
        }
    }
    if (count > 0)
        m_bitmap.free_bits_batch(slots, count);
}

void pool_view::reset() noexcept
{
    m_bitmap.reset();
}

size_t pool_view::free_count() const noexcept  { return m_bitmap.free_count(); }
size_t pool_view::block_count() const noexcept { return m_block_count; }
size_t pool_view::block_size() const noexcept  { return m_block_size; }
size_t pool_view::capacity() const noexcept    { return m_block_size * m_block_count; }

bool pool_view::owns(const void* ptr) const noexcept
{
    if (ptr == nullptr || m_memory == nullptr) return false;
    auto byte_ptr = static_cast<const std::byte*>(ptr);
    if (byte_ptr < m_memory || byte_ptr >= m_memory + m_block_size * m_block_count) return false;
    return (static_cast<size_t>(byte_ptr - m_memory) & (m_block_size - 1)) == 0;
}

bool pool_view::is_initialized() const noexcept { return m_memory != nullptr; }

std::byte* pool_view::memory_start() const noexcept { return m_memory; }
std::byte* pool_view::memory_end() const noexcept
{
    return m_memory ? m_memory + m_block_size * m_block_count : nullptr;
}

} // namespace AL
