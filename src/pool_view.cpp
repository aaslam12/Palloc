#include "pool_view.h"
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
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
    m_free_count.store(block_count, std::memory_order_relaxed);

    m_bitmap_words = (block_count + 63) / 64;
    m_block_shift = static_cast<size_t>(std::countr_zero(block_size));

    m_bitmap = static_cast<AL::palloc_atomic<uint64_t>*>(base);
    std::memset(m_bitmap, 0, m_bitmap_words * sizeof(uint64_t));

    // mark trailing bits beyond m_block_count as allocated
    size_t tail = m_block_count % 64;
    if (tail != 0)
        m_bitmap[m_bitmap_words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);

    m_hint.store(0, std::memory_order_relaxed);

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
    // scan from hint first (fast path). If that misses due to concurrent frees
    // behind the hint, fall back to a full scan from 0.
    for (size_t pass = 0; pass < 2; pass++)
    {
        size_t hint = m_hint.load(std::memory_order_relaxed);
        size_t start = (pass == 0) ? hint : 0;
        size_t stop = (pass == 0) ? m_bitmap_words : hint;
        if (start >= m_bitmap_words)
        {
            stop = 0;
        } // hint past end, skip pass 0

        for (size_t w = start; w < ((pass == 0) ? m_bitmap_words : stop); ++w)
        {
            uint64_t word = m_bitmap[w].load(std::memory_order_relaxed);
            while (word != ~uint64_t(0))
            {
                size_t bit = static_cast<size_t>(std::countr_zero(~word));
                size_t block_idx = w * 64 + bit;

                if (block_idx >= m_block_count)
                    return nullptr;

                uint64_t new_word = word | (uint64_t(1) << bit);
                if (m_bitmap[w].compare_exchange_weak(word, new_word, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    m_free_count.fetch_sub(1, std::memory_order_relaxed);
                    if (new_word == ~uint64_t(0))
                        m_hint.store(w + 1, std::memory_order_relaxed);
                    return m_memory + (block_idx << m_block_shift);
                }
            }
        }
    }

    return nullptr;
}

size_t pool_view::alloc_batch(size_t count, void* out[]) noexcept
{
    if (count == 0)
        return 0;

    size_t found = 0;

    for (size_t w = 0; w < m_bitmap_words && found < count; ++w)
    {
        uint64_t word = m_bitmap[w].load(std::memory_order_relaxed);
        if (word == ~uint64_t(0))
            continue;

        uint64_t new_word;
        uint64_t claimed = 0;
        size_t local_found = 0;

        do
        {
            claimed = 0;
            local_found = 0;
            uint64_t free_bits = ~word;
            size_t tmp_found = found;

            while (free_bits && tmp_found < count)
            {
                size_t bit = static_cast<size_t>(std::countr_zero(free_bits));
                size_t block_idx = w * 64 + bit;

                if (block_idx >= m_block_count)
                {
                    free_bits = 0;
                    break;
                }

                claimed |= (uint64_t(1) << bit);
                free_bits &= free_bits - 1;
                ++tmp_found;
                ++local_found;
            }

            if (claimed == 0)
                break;

            new_word = word | claimed;
        }
        while (!m_bitmap[w].compare_exchange_weak(word, new_word, std::memory_order_acquire, std::memory_order_relaxed));

        if (claimed == 0)
            continue;

        // claimed bits are now ours, populate out[]
        uint64_t bits = claimed;
        while (bits)
        {
            size_t bit = static_cast<size_t>(std::countr_zero(bits));
            size_t block_idx = w * 64 + bit;
            out[found++] = m_memory + (block_idx << m_block_shift);
            bits &= bits - 1;
        }

        m_free_count.fetch_sub(local_found, std::memory_order_relaxed);
    }

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

    uint64_t prev = m_bitmap[word_idx].fetch_and(~mask, std::memory_order_release);
    assert((prev & mask) != 0 && "double free: block is not currently allocated");
    (void)prev;

    m_free_count.fetch_add(1, std::memory_order_relaxed);
}

void pool_view::free_batch(std::span<void*> ptrs) noexcept
{
    size_t count = 0;

    // accumulate masks per word, then write one atomic per touched word
    // first pass: group by word
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

        m_bitmap[word_idx].fetch_and(~mask, std::memory_order_release);
        ++count;
    }

    if (count > 0)
        m_free_count.fetch_add(count, std::memory_order_relaxed);
}

void pool_view::reset() noexcept
{
    std::memset(m_bitmap, 0, m_bitmap_words * sizeof(uint64_t));

    size_t tail = m_block_count % 64;
    if (tail != 0)
        m_bitmap[m_bitmap_words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);

    m_free_count.store(m_block_count, std::memory_order_relaxed);
    m_hint.store(0, std::memory_order_relaxed);
}

size_t pool_view::free_count() const noexcept
{
    return m_free_count.load(std::memory_order_relaxed);
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
