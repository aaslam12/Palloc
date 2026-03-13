#include "pool_view.h"
#include <bit>
#include <cassert>
#include <cstring>
#include <memory>

namespace AL
{

size_t pool_view::required_region_size(size_t block_size, size_t block_count) noexcept
{
    size_t bitmap_words = (block_count + 63) / 64;
    size_t bitmap_bytes = bitmap_words * sizeof(uint64_t);
    // payload must start at block_size alignment
    size_t aligned_offset = ((bitmap_bytes + block_size - 1) / block_size) * block_size;
    return aligned_offset + block_size * block_count;
}

void pool_view::init_from_region(void* base, size_t block_size, size_t block_count) noexcept
{
    assert(base != nullptr && "base must not be null");
    assert(block_size > 0 && std::has_single_bit(block_size) && "block_size must be a power of 2");
    assert(block_size >= sizeof(void*) && "block_size must be at least sizeof(void*)");
    assert(block_count > 0 && "block_count must be positive");
    assert((reinterpret_cast<uintptr_t>(base) % block_size) == 0 && "base must be aligned to at least block_size");

    m_block_size = block_size;
    m_block_count = block_count;
    m_free_count = block_count;
    m_bitmap_words = (block_count + 63) / 64;
    m_hint = 0;
    m_block_shift = static_cast<size_t>(std::countr_zero(block_size));

    m_bitmap = static_cast<uint64_t*>(base);
    std::memset(m_bitmap, 0, m_bitmap_words * sizeof(uint64_t));

    // mark trailing bits beyond m_block_count as allocated so the hint
    // can advance past the last word when all valid blocks are used
    size_t tail = m_block_count % 64;
    if (tail != 0)
        m_bitmap[m_bitmap_words - 1] = ~uint64_t(0) << tail;

    // align payload to block_size
    size_t bitmap_bytes = m_bitmap_words * sizeof(uint64_t);
    void* payload_ptr = static_cast<std::byte*>(base) + bitmap_bytes;
    size_t remaining = required_region_size(block_size, block_count) - bitmap_bytes;

    void* aligned = std::align(block_size, block_size * block_count, payload_ptr, remaining);
    assert(aligned != nullptr && "failed to align payload region");

    m_memory = static_cast<std::byte*>(aligned);
}

void* pool_view::alloc() noexcept
{
    if (m_free_count == 0)
        return nullptr;

    for (size_t w = m_hint; w < m_bitmap_words; ++w)
    {
        uint64_t word = m_bitmap[w];
        if (word == ~uint64_t(0))
            continue; // all bits set (all allocated)

        size_t bit = static_cast<size_t>(std::countr_zero(static_cast<uint64_t>(~word)));
        size_t block_idx = w * 64 + bit;

        if (block_idx >= m_block_count)
            return nullptr;

        m_bitmap[w] |= (uint64_t(1) << bit);
        --m_free_count;

        // advance hint past full words
        if (m_bitmap[w] == ~uint64_t(0))
            m_hint = w + 1;

        return m_memory + (block_idx << m_block_shift);
    }

    return nullptr;
}

size_t pool_view::alloc_batch(size_t count, void* out[]) noexcept
{
    if (m_free_count == 0 || count == 0)
        return 0;

    size_t found = 0;

    for (size_t w = m_hint; w < m_bitmap_words && found < count; ++w)
    {
        uint64_t word = m_bitmap[w];
        if (word == ~uint64_t(0))
            continue;

        uint64_t free_bits = ~word;
        uint64_t new_alloc = 0;

        while (free_bits && found < count)
        {
            size_t bit = static_cast<size_t>(std::countr_zero(free_bits));
            size_t block_idx = w * 64 + bit;

            // trailing bits are pre-set allocated; this is a safety guard
            if (block_idx >= m_block_count)
            {
                free_bits = 0;
                break;
            }

            out[found++] = m_memory + (block_idx << m_block_shift);
            uint64_t b = uint64_t(1) << bit;
            new_alloc |= b;
            free_bits &= free_bits - 1; // clear lowest set bit
        }

        if (new_alloc)
        {
            m_bitmap[w] |= new_alloc;
            if (m_bitmap[w] == ~uint64_t(0))
                m_hint = w + 1;
        }
    }

    m_free_count -= found;
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
    if (ptr == nullptr)
        return;

    assert(owns(ptr) && "pointer does not belong to this pool_view");

    auto byte_ptr = static_cast<std::byte*>(ptr);
    size_t offset = static_cast<size_t>(byte_ptr - m_memory);
    size_t block_idx = offset >> m_block_shift;

    size_t word_idx = block_idx >> 6;
    size_t bit_idx = block_idx & 63;

    uint64_t mask = uint64_t(1) << bit_idx;
    assert((m_bitmap[word_idx] & mask) != 0 && "double free: block is not currently allocated");

    if (!(m_bitmap[word_idx] & mask))
        return; // block already free — no-op to prevent state corruption

    m_bitmap[word_idx] &= ~mask;
    ++m_free_count;

    // pull hint back so alloc can find this word
    if (word_idx < m_hint)
        m_hint = word_idx;
}

void pool_view::free_batch(std::span<void*> ptrs) noexcept
{
    size_t count = 0;
    size_t min_hint = m_hint;

    for (void* ptr : ptrs)
    {
        if (ptr == nullptr)
            continue;

        assert(owns(ptr) && "pointer does not belong to this pool_view");

        auto byte_ptr = static_cast<std::byte*>(ptr);
        size_t offset = static_cast<size_t>(byte_ptr - m_memory);
        size_t block_idx = offset >> m_block_shift;
        size_t word_idx = block_idx >> 6;
        size_t bit_idx = block_idx & 63;

        uint64_t mask = uint64_t(1) << bit_idx;
        if (!(m_bitmap[word_idx] & mask))
            continue; // already free

        m_bitmap[word_idx] &= ~mask;
        ++count;

        if (word_idx < min_hint)
            min_hint = word_idx;
    }

    m_free_count += count;
    if (min_hint < m_hint)
        m_hint = min_hint;
}

void pool_view::reset() noexcept
{
    std::memset(m_bitmap, 0, m_bitmap_words * sizeof(uint64_t));

    size_t tail = m_block_count % 64;
    if (tail != 0)
        m_bitmap[m_bitmap_words - 1] = ~uint64_t(0) << tail;

    m_free_count = m_block_count;
    m_hint = 0;
}

size_t pool_view::free_count() const noexcept
{
    return m_free_count;
}

size_t pool_view::block_count() const noexcept
{
    return m_block_count;
}

size_t pool_view::block_size() const noexcept
{
    return m_block_size;
}

size_t pool_view::capacity() const noexcept
{
    return m_block_size * m_block_count;
}

bool pool_view::owns(const void* ptr) const noexcept
{
    if (ptr == nullptr || m_memory == nullptr)
        return false;

    auto byte_ptr = static_cast<const std::byte*>(ptr);
    if (byte_ptr < m_memory || byte_ptr >= m_memory + m_block_size * m_block_count)
        return false;

    size_t offset = static_cast<size_t>(byte_ptr - m_memory);
    return (offset & (m_block_size - 1)) == 0;
}

bool pool_view::is_initialized() const noexcept
{
    return m_memory != nullptr;
}

std::byte* pool_view::memory_start() const noexcept
{
    return m_memory;
}

std::byte* pool_view::memory_end() const noexcept
{
    return m_memory ? m_memory + m_block_size * m_block_count : nullptr;
}

} // namespace AL
