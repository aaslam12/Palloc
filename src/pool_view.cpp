#include "pool_view.h"
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>

namespace AL
{

// bitmap helpers

static size_t bitmap_alloc_bit(palloc_atomic<uint64_t>* words, size_t num_words, size_t num_slots, palloc_atomic<size_t>& hint) noexcept
{
    for (int pass = 0; pass < 2; ++pass)
    {
        size_t h = hint.load(std::memory_order_relaxed);
        size_t start = (pass == 0) ? h : 0;
        size_t stop = (pass == 0) ? num_words : h;

        if (start >= num_words)
            stop = 0;

        for (size_t w = start; w < (pass == 0 ? num_words : stop); ++w)
        {
            uint64_t word = words[w].load(std::memory_order_relaxed);

            while (true)
            {
                if (word == ~uint64_t(0))
                    break;

                size_t bit = static_cast<size_t>(std::countr_zero(~word));
                size_t slot = w * 64 + bit;

                if (slot >= num_slots)
                    return static_cast<size_t>(-1);

                uint64_t new_word = word | (uint64_t(1) << bit);

                if (words[w].compare_exchange_weak(word, new_word, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    if (new_word == ~uint64_t(0))
                        hint.store(w + 1, std::memory_order_relaxed);

                    return slot;
                }
            }
        }
    }
    return static_cast<size_t>(-1);
}

static size_t bitmap_alloc_batch(palloc_atomic<uint64_t>* words, size_t num_words, size_t num_slots, size_t count, size_t out[]) noexcept
{
    size_t found = 0;

    for (size_t w = 0; w < num_words && found < count; ++w)
    {
        uint64_t word = words[w].load(std::memory_order_relaxed);
        uint64_t claimed = 0;
        size_t local = 0;
        uint64_t new_word = 0;

        do
        {
            claimed = 0;
            local = 0;

            if (word == ~uint64_t(0))
                break;

            uint64_t free_bits = ~word;
            size_t tmp = found;

            while (free_bits && tmp < count)
            {
                size_t bit = static_cast<size_t>(std::countr_zero(free_bits));
                size_t slot = w * 64 + bit;

                if (slot >= num_slots)
                {
                    free_bits = 0;
                    break;
                }

                claimed |= uint64_t(1) << bit;
                free_bits &= free_bits - 1;
                ++tmp;
                ++local;
            }

            if (!claimed)
                break;

            new_word = word | claimed;
        }
        while (!words[w].compare_exchange_weak(word, new_word, std::memory_order_acquire, std::memory_order_relaxed));

        if (!claimed)
            continue;

        uint64_t bits = claimed;
        while (bits)
        {
            size_t bit = static_cast<size_t>(std::countr_zero(bits));
            out[found++] = w * 64 + bit;
            bits &= bits - 1;
        }
    }
    return found;
}

static void bitmap_free_bit(palloc_atomic<uint64_t>* words, size_t slot) noexcept
{
    size_t w = slot >> 6;
    words[w].fetch_and(~(uint64_t(1) << (slot & 63)), std::memory_order_release);
}

static bool bitmap_is_empty(const palloc_atomic<uint64_t>* words, size_t num_words) noexcept
{
    for (size_t w = 0; w < num_words; ++w)
        if (words[w].load(std::memory_order_relaxed) != 0)
            return false;
    return true;
}

// static helpers

size_t pool_view::required_region_size(size_t block_size, size_t block_count) noexcept
{
    size_t bitmap_bytes = ((block_count + 63) / 64) * sizeof(uint64_t);
    size_t aligned_offset = ((bitmap_bytes + block_size - 1) / block_size) * block_size;
    return aligned_offset + block_size * block_count;
}

size_t pool_view::fine_bitmap_bytes(size_t blocks_per_chunk) noexcept
{
    return ((blocks_per_chunk + 63) / 64) * sizeof(uint64_t);
}

// move

pool_view::pool_view(pool_view&& other) noexcept
    : m_memory(other.m_memory), m_block_size(other.m_block_size), m_block_shift(other.m_block_shift), m_blocks_per_chunk(other.m_blocks_per_chunk),
      m_words_per_chunk(other.m_words_per_chunk), m_committed_blocks(other.m_committed_blocks.load(std::memory_order_relaxed)),
      m_reserved_blocks(other.m_reserved_blocks.load(std::memory_order_relaxed)), m_virtual_block_ceiling(other.m_virtual_block_ceiling),
      m_free_count(other.m_free_count.load(std::memory_order_relaxed)), m_chunk_bitmaps(other.m_chunk_bitmaps),
      m_embedded_bitmap(other.m_embedded_bitmap), m_embedded_num_words(other.m_embedded_num_words),
      m_hint(other.m_hint.load(std::memory_order_relaxed))
{
    other.m_memory = nullptr;
}

pool_view& pool_view::operator=(pool_view&& other) noexcept
{
    if (this == &other)
        return *this;
    m_memory = other.m_memory;
    m_block_size = other.m_block_size;
    m_block_shift = other.m_block_shift;
    m_blocks_per_chunk = other.m_blocks_per_chunk;
    m_words_per_chunk = other.m_words_per_chunk;
    m_committed_blocks.store(other.m_committed_blocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_reserved_blocks.store(other.m_reserved_blocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_virtual_block_ceiling = other.m_virtual_block_ceiling;
    m_free_count.store(other.m_free_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_chunk_bitmaps = other.m_chunk_bitmaps;
    m_embedded_bitmap = other.m_embedded_bitmap;
    m_embedded_num_words = other.m_embedded_num_words;
    m_hint.store(other.m_hint.load(std::memory_order_relaxed), std::memory_order_relaxed);
    other.m_memory = nullptr;
    return *this;
}

// init

void pool_view::init_from_region(void* base, size_t block_size, size_t block_count) noexcept
{
    assert(base && block_size > 0 && std::has_single_bit(block_size) && block_size >= sizeof(void*) && block_count > 0);
    assert((reinterpret_cast<uintptr_t>(base) % block_size) == 0);

    m_block_size = block_size;
    m_block_shift = static_cast<size_t>(std::countr_zero(block_size));
    m_blocks_per_chunk = block_count;
    m_words_per_chunk = (block_count + 63) / 64;
    m_committed_blocks.store(block_count, std::memory_order_relaxed);
    m_reserved_blocks.store(block_count, std::memory_order_relaxed);
    m_virtual_block_ceiling = block_count;
    m_free_count.store(block_count, std::memory_order_relaxed);
    m_hint.store(0, std::memory_order_relaxed);

    // bitmap embedded at start of region
    size_t bitmap_bytes = m_words_per_chunk * sizeof(uint64_t);
    std::memset(base, 0, bitmap_bytes);
    m_embedded_bitmap = static_cast<palloc_atomic<uint64_t>*>(base);
    m_embedded_num_words = m_words_per_chunk;
    m_chunk_bitmaps = nullptr;

    // tail bits
    size_t tail = block_count % 64;
    if (tail)
        m_embedded_bitmap[m_words_per_chunk - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);

    // payload after bitmap, aligned
    void* payload = static_cast<std::byte*>(base) + bitmap_bytes;
    size_t rem = required_region_size(block_size, block_count) - bitmap_bytes;
    void* aligned = std::align(block_size, block_size * block_count, payload, rem);
    assert(aligned);

    m_memory = static_cast<std::byte*>(aligned);
}

void pool_view::init_from_region(void* base,
                                 palloc_atomic<uint64_t>** chunk_bitmaps,
                                 size_t block_size,
                                 size_t committed_blocks,
                                 size_t virtual_block_ceiling,
                                 size_t blocks_per_chunk) noexcept
{
    assert(base && chunk_bitmaps && block_size > 0 && std::has_single_bit(block_size) && block_size >= sizeof(void*) && committed_blocks > 0 &&
           virtual_block_ceiling >= committed_blocks);
    assert((reinterpret_cast<uintptr_t>(base) % block_size) == 0);

    m_block_size = block_size;
    m_block_shift = static_cast<size_t>(std::countr_zero(block_size));
    m_blocks_per_chunk = blocks_per_chunk;
    m_words_per_chunk = (blocks_per_chunk + 63) / 64;

    m_committed_blocks.store(committed_blocks, std::memory_order_relaxed);
    m_reserved_blocks.store(committed_blocks, std::memory_order_relaxed);
    m_virtual_block_ceiling = virtual_block_ceiling;
    m_free_count.store(committed_blocks, std::memory_order_relaxed);
    m_hint.store(0, std::memory_order_relaxed);

    m_chunk_bitmaps = chunk_bitmaps;
    m_embedded_bitmap = nullptr;
    m_embedded_num_words = 0;
    m_memory = static_cast<std::byte*>(base);
}

// alloc/free internals

size_t pool_view::alloc_from_chunk(size_t chunk_idx) noexcept
{
    palloc_atomic<uint64_t>* words = m_chunk_bitmaps[chunk_idx];

    if (!words)
        return static_cast<size_t>(-1);

    // per-chunk hint: use global hint shifted to chunk
    size_t tail = m_blocks_per_chunk % 64;
    // mask tail bits in last word
    size_t last_w = m_words_per_chunk - 1;

    for (size_t w = 0; w < m_words_per_chunk; ++w)
    {
        uint64_t word = words[w].load(std::memory_order_relaxed);

        while (true)
        {
            uint64_t effective = (w == last_w && tail) ? (word | (~uint64_t(0) << tail)) : word;

            if (effective == ~uint64_t(0))
                break;

            size_t bit = static_cast<size_t>(std::countr_zero(~effective));
            size_t slot = w * 64 + bit;

            if (slot >= m_blocks_per_chunk)
                return static_cast<size_t>(-1);

            uint64_t new_word = word | (uint64_t(1) << bit);

            if (words[w].compare_exchange_weak(word, new_word, std::memory_order_acquire, std::memory_order_relaxed))
                return chunk_idx * m_blocks_per_chunk + slot;
        }
    }
    return static_cast<size_t>(-1);
}

size_t pool_view::alloc_batch_from_chunk(size_t chunk_idx, size_t count, size_t out[]) noexcept
{
    palloc_atomic<uint64_t>* words = m_chunk_bitmaps[chunk_idx];
    if (!words)
        return 0;

    size_t tail = m_blocks_per_chunk % 64;
    size_t last_w = m_words_per_chunk - 1;
    size_t found = 0;

    for (size_t w = 0; w < m_words_per_chunk && found < count; ++w)
    {
        uint64_t word = words[w].load(std::memory_order_relaxed);
        uint64_t claimed = 0;
        size_t local = 0;
        uint64_t new_word = 0;

        do
        {
            claimed = 0;
            local = 0;
            uint64_t effective = (w == last_w && tail) ? (word | (~uint64_t(0) << tail)) : word;

            if (effective == ~uint64_t(0))
                break;

            uint64_t free_bits = ~effective;
            size_t tmp = found;

            while (free_bits && tmp < count)
            {
                size_t bit = static_cast<size_t>(std::countr_zero(free_bits));
                size_t slot = w * 64 + bit;

                if (slot >= m_blocks_per_chunk)
                {
                    free_bits = 0;
                    break;
                }

                claimed |= uint64_t(1) << bit;
                free_bits &= free_bits - 1;
                ++tmp;
                ++local;
            }

            if (!claimed)
                break;
            new_word = word | claimed;
        }
        while (!words[w].compare_exchange_weak(word, new_word, std::memory_order_acquire, std::memory_order_relaxed));

        if (!claimed)
            continue;

        uint64_t bits = claimed;
        while (bits)
        {
            size_t bit = static_cast<size_t>(std::countr_zero(bits));
            out[found++] = chunk_idx * m_blocks_per_chunk + w * 64 + bit;
            bits &= bits - 1;
        }
    }
    return found;
}

void pool_view::free_to_chunk(size_t chunk_idx, size_t bit_within_chunk) noexcept
{
    palloc_atomic<uint64_t>* words = m_chunk_bitmaps[chunk_idx];
    assert(words);
    size_t w = bit_within_chunk >> 6;
    words[w].fetch_and(~(uint64_t(1) << (bit_within_chunk & 63)), std::memory_order_release);
}

// public alloc/free

void* pool_view::alloc() noexcept
{
    size_t slot;
    if (m_chunk_bitmaps)
    {
        // two-level: scan committed chunks via m_chunk_bitmaps
        size_t committed = m_committed_blocks.load(std::memory_order_acquire);
        size_t num_chunks = (committed + m_blocks_per_chunk - 1) / m_blocks_per_chunk;

        // scan from hint chunk
        size_t hint_chunk = m_hint.load(std::memory_order_relaxed);
        for (int pass = 0; pass < 2; ++pass)
        {
            size_t start = (pass == 0) ? hint_chunk : 0;
            size_t stop = (pass == 0) ? num_chunks : hint_chunk;
            if (start >= num_chunks)
                stop = 0;

            for (size_t c = start; c < (pass == 0 ? num_chunks : stop); ++c)
            {
                slot = alloc_from_chunk(c);
                if (slot != static_cast<size_t>(-1))
                {
                    m_free_count.fetch_sub(1, std::memory_order_relaxed);
                    return m_memory + (slot << m_block_shift);
                }
                else
                    m_hint.store(c + 1, std::memory_order_relaxed);
            }
        }
        return nullptr;
    }
    else
    {
        // embedded single-chunk path
        slot = bitmap_alloc_bit(m_embedded_bitmap, m_embedded_num_words, m_blocks_per_chunk, m_hint);

        if (slot == static_cast<size_t>(-1))
            return nullptr;

        m_free_count.fetch_sub(1, std::memory_order_relaxed);
        return m_memory + (slot << m_block_shift);
    }
}

size_t pool_view::alloc_batch(size_t count, void* out[]) noexcept
{
    size_t slots[128];
    size_t n = count > 128 ? 128 : count;
    size_t found = 0;

    if (m_chunk_bitmaps)
    {
        size_t committed = m_committed_blocks.load(std::memory_order_acquire);
        size_t num_chunks = (committed + m_blocks_per_chunk - 1) / m_blocks_per_chunk;

        for (size_t c = 0; c < num_chunks && found < n; ++c)
            found += alloc_batch_from_chunk(c, n - found, slots + found);
    }
    else
    {
        found = bitmap_alloc_batch(m_embedded_bitmap, m_embedded_num_words, m_blocks_per_chunk, n, slots);
    }

    m_free_count.fetch_sub(found, std::memory_order_relaxed);

    for (size_t i = 0; i < found; ++i)
        out[i] = m_memory + (slots[i] << m_block_shift);
    return found;
}

void* pool_view::calloc() noexcept
{
    void* ptr = alloc();
    if (ptr)
        std::memset(ptr, 0, m_block_size);
    return ptr;
}

void pool_view::free(void* ptr) noexcept
{
    if (!ptr)
        return;
    assert(owns(ptr));
    size_t slot = static_cast<size_t>(static_cast<std::byte*>(ptr) - m_memory) >> m_block_shift;
    if (m_chunk_bitmaps)
    {
        size_t chunk_idx = slot / m_blocks_per_chunk;
        free_to_chunk(chunk_idx, slot % m_blocks_per_chunk);
    }
    else
        bitmap_free_bit(m_embedded_bitmap, slot);
    m_free_count.fetch_add(1, std::memory_order_relaxed);
}

void pool_view::free_batch(std::span<void*> ptrs) noexcept
{
    if (m_chunk_bitmaps)
    {
        // accumulate masks per (chunk, word) then flush with one fetch_and per word
        struct entry
        {
            size_t chunk;
            size_t word;
            uint64_t mask;
        };

        std::array<entry, 128> entries;
        size_t n = 0;
        for (void* ptr : ptrs)
        {
            if (!ptr)
                continue;
            assert(owns(ptr));
            size_t slot = static_cast<size_t>(static_cast<std::byte*>(ptr) - m_memory) >> m_block_shift;
            size_t chunk = slot / m_blocks_per_chunk;
            size_t bit = slot % m_blocks_per_chunk;
            size_t word = bit >> 6;
            uint64_t mask = uint64_t(1) << (bit & 63);
            bool merged = false;
            for (size_t i = 0; i < n; ++i)
            {
                if (entries[i].chunk == chunk && entries[i].word == word)
                {
                    entries[i].mask |= mask;
                    merged = true;
                    break;
                }
            }
            if (!merged && n < 128)
                entries[n++] = {.chunk = chunk, .word = word, .mask = mask};
        }

        for (size_t i = 0; i < n; ++i)
        {
            palloc_atomic<uint64_t>* words = m_chunk_bitmaps[entries[i].chunk];
            if (words)
                words[entries[i].word].fetch_and(~entries[i].mask, std::memory_order_release);
        }
    }
    else
    {
        for (void* ptr : ptrs)
        {
            if (!ptr)
                continue;

            assert(owns(ptr));

            size_t slot = static_cast<size_t>(static_cast<std::byte*>(ptr) - m_memory) >> m_block_shift;
            bitmap_free_bit(m_embedded_bitmap, slot);
        }
    }
    m_free_count.fetch_add(ptrs.size(), std::memory_order_relaxed);
}

void pool_view::reset() noexcept
{
    if (m_chunk_bitmaps)
    {
        size_t committed = m_committed_blocks.load(std::memory_order_relaxed);
        size_t num_chunks = (committed + m_blocks_per_chunk - 1) / m_blocks_per_chunk;

        for (size_t c = 0; c < num_chunks; ++c)
        {
            palloc_atomic<uint64_t>* words = m_chunk_bitmaps[c];
            if (!words)
                continue;

            size_t tail = m_blocks_per_chunk % 64;
            std::memset(words, 0, m_words_per_chunk * sizeof(uint64_t));

            if (tail)
                words[m_words_per_chunk - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);
        }

        m_free_count.store(committed, std::memory_order_relaxed);
    }
    else
    {
        size_t tail = m_blocks_per_chunk % 64;
        std::memset(m_embedded_bitmap, 0, m_embedded_num_words * sizeof(uint64_t));

        if (tail)
            m_embedded_bitmap[m_embedded_num_words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);

        m_free_count.store(m_blocks_per_chunk, std::memory_order_relaxed);
    }
    m_hint.store(0, std::memory_order_relaxed);
}

// growth

bool pool_view::try_reserve_chunk(size_t& old_committed) noexcept
{
    old_committed = m_committed_blocks.load(std::memory_order_acquire);
    size_t next = old_committed + m_blocks_per_chunk;

    if (next > m_virtual_block_ceiling)
        return false;

    size_t expected = old_committed;
    return m_reserved_blocks.compare_exchange_strong(expected, next, std::memory_order_acquire, std::memory_order_relaxed);
}

void pool_view::advance_committed(size_t new_committed) noexcept
{
    size_t added = new_committed - m_committed_blocks.load(std::memory_order_relaxed);
    m_free_count.fetch_add(added, std::memory_order_relaxed);
    m_committed_blocks.store(new_committed, std::memory_order_release);
}

// shrink helpers

bool pool_view::is_chunk_empty(size_t chunk_idx) const noexcept
{
    palloc_atomic<uint64_t>* words = m_chunk_bitmaps ? m_chunk_bitmaps[chunk_idx] : nullptr;
    if (!words)
        return true; // already decommitted

    return bitmap_is_empty(words, m_words_per_chunk);
}

void pool_view::decommit_blocks(size_t new_committed) noexcept
{
    size_t old = m_committed_blocks.load(std::memory_order_relaxed);
    if (old > new_committed)
        m_free_count.fetch_sub(old - new_committed, std::memory_order_relaxed);

    m_reserved_blocks.store(new_committed, std::memory_order_relaxed);
    m_committed_blocks.store(new_committed, std::memory_order_relaxed);
}

// accessors

size_t pool_view::free_count() const noexcept
{
    return m_free_count.load(std::memory_order_relaxed);
}

size_t pool_view::block_count() const noexcept
{
    return m_committed_blocks.load(std::memory_order_relaxed);
}

size_t pool_view::block_size() const noexcept
{
    return m_block_size;
}

size_t pool_view::capacity() const noexcept
{
    return m_block_size * m_committed_blocks.load(std::memory_order_relaxed);
}

size_t pool_view::committed_blocks() const noexcept
{
    return m_committed_blocks.load(std::memory_order_relaxed);
}

size_t pool_view::virtual_block_ceiling() const noexcept
{
    return m_virtual_block_ceiling;
}

size_t pool_view::blocks_per_chunk() const noexcept
{
    return m_blocks_per_chunk;
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
    if (!m_memory)
        return nullptr;
    return m_memory + m_block_size * m_committed_blocks.load(std::memory_order_relaxed);
}

bool pool_view::owns(const void* ptr) const noexcept
{
    if (!ptr || !m_memory)
        return false;
    auto p = static_cast<const std::byte*>(ptr);
    size_t committed = m_committed_blocks.load(std::memory_order_relaxed);

    if (p < m_memory || p >= m_memory + m_block_size * committed)
        return false;

    return (static_cast<size_t>(p - m_memory) & (m_block_size - 1)) == 0;
}

} // namespace AL
