#include "pool_view.h"
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>

namespace AL
{

// ─── bitmap helpers ───────────────────────────────────────────────────────────

// alloc one bit from [words, words+num_words), scanning only up to num_slots.
// tl_hint is a thread-local starting word that advances to spread threads across the bitmap.
// returns slot index or -1 if full.
static size_t bitmap_alloc_bit(palloc_atomic<uint64_t>* words,
                                size_t num_words,
                                size_t num_slots,
                                size_t& tl_hint) noexcept
{
    for (int pass = 0; pass < 2; ++pass)
    {
        size_t start = (pass == 0) ? tl_hint : 0;
        size_t stop  = (pass == 0) ? num_words : tl_hint;

        if (start >= num_words)
            stop = 0;

        for (size_t w = start; w < (pass == 0 ? num_words : stop); ++w)
        {
            uint64_t word = words[w].load(std::memory_order_relaxed);

            while (word != ~uint64_t(0))
            {
                size_t bit  = static_cast<size_t>(std::countr_zero(~word));
                size_t slot = w * 64 + bit;

                if (slot >= num_slots)
                    return static_cast<size_t>(-1);

                uint64_t mask = uint64_t(1) << bit;
                uint64_t old  = words[w].fetch_or(mask, std::memory_order_acquire);

                if (!(old & mask))
                {
                    // we claimed the bit
                    if ((old | mask) == ~uint64_t(0))
                        tl_hint = w + 1; // advance past full word
                    return slot;
                }
                // another thread took this bit; re-read and try the next free bit
                word = old | mask;
            }
        }
    }
    return static_cast<size_t>(-1);
}

static size_t bitmap_alloc_batch(palloc_atomic<uint64_t>* words,
                                  size_t num_words,
                                  size_t num_slots,
                                  size_t count,
                                  size_t out[],
                                  size_t& tl_hint) noexcept
{
    size_t found = 0;

    // two-pass scan starting from tl_hint
    for (int pass = 0; pass < 2 && found < count; ++pass)
    {
        size_t start = (pass == 0) ? tl_hint : 0;
        size_t stop  = (pass == 0) ? num_words : tl_hint;
        if (start >= num_words)
            stop = 0;

        for (size_t w = start; w < (pass == 0 ? num_words : stop) && found < count; ++w)
        {
            uint64_t word    = words[w].load(std::memory_order_relaxed);
            uint64_t claimed = 0;
            size_t   local   = 0;
            uint64_t new_word = 0;

            do
            {
                claimed = 0;
                local   = 0;

                if (word == ~uint64_t(0))
                    break;

                uint64_t free_bits = ~word;
                size_t   tmp       = found;

                while (free_bits && tmp < count)
                {
                    size_t bit  = static_cast<size_t>(std::countr_zero(free_bits));
                    size_t slot = w * 64 + bit;

                    if (slot >= num_slots)
                    {
                        free_bits = 0;
                        break;
                    }

                    claimed    |= uint64_t(1) << bit;
                    free_bits  &= free_bits - 1;
                    ++tmp;
                    ++local;
                }

                if (!claimed)
                    break;

                new_word = word | claimed;
            }
            while (!words[w].compare_exchange_weak(word, new_word,
                        std::memory_order_acquire, std::memory_order_relaxed));

            if (!claimed)
                continue;

            if (new_word == ~uint64_t(0))
                tl_hint = w + 1; // advance past full word

            uint64_t bits = claimed;
            while (bits)
            {
                size_t bit = static_cast<size_t>(std::countr_zero(bits));
                out[found++] = w * 64 + bit;
                bits &= bits - 1;
            }
        }
    }
    return found;
}

static void bitmap_free_bit(palloc_atomic<uint64_t>* words, size_t slot) noexcept
{
    size_t w = slot >> 6;
    words[w].fetch_and(~(uint64_t(1) << (slot & 63)), std::memory_order_release);
}

static bool bitmap_is_word_range_empty(const palloc_atomic<uint64_t>* words,
                                        size_t word_start, size_t word_end) noexcept
{
    for (size_t w = word_start; w < word_end; ++w)
        if (words[w].load(std::memory_order_relaxed) != 0)
            return false;
    return true;
}

// ─── static helpers ───────────────────────────────────────────────────────────

size_t pool_view::required_region_size(size_t block_size, size_t block_count) noexcept
{
    size_t bitmap_bytes   = ((block_count + 63) / 64) * sizeof(uint64_t);
    size_t aligned_offset = ((bitmap_bytes + block_size - 1) / block_size) * block_size;
    return aligned_offset + block_size * block_count;
}

size_t pool_view::bitmap_bytes_for(size_t block_count) noexcept
{
    return ((block_count + 63) / 64) * sizeof(uint64_t);
}

// ─── move ─────────────────────────────────────────────────────────────────────

pool_view::pool_view(pool_view&& other) noexcept
    : m_memory(other.m_memory),
      m_block_size(other.m_block_size),
      m_block_shift(other.m_block_shift),
      m_blocks_per_chunk(other.m_blocks_per_chunk),
      m_committed_blocks(other.m_committed_blocks.load(std::memory_order_relaxed)),
      m_reserved_blocks(other.m_reserved_blocks.load(std::memory_order_relaxed)),
      m_virtual_block_ceiling(other.m_virtual_block_ceiling),
      m_free_count(other.m_free_count.load(std::memory_order_relaxed)),
      m_bitmap(other.m_bitmap),
      m_bitmap_words(other.m_bitmap_words)
{
    other.m_memory = nullptr;
}

pool_view& pool_view::operator=(pool_view&& other) noexcept
{
    if (this == &other)
        return *this;
    m_memory      = other.m_memory;
    m_block_size  = other.m_block_size;
    m_block_shift = other.m_block_shift;
    m_blocks_per_chunk = other.m_blocks_per_chunk;
    m_committed_blocks.store(other.m_committed_blocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_reserved_blocks.store(other.m_reserved_blocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_virtual_block_ceiling = other.m_virtual_block_ceiling;
    m_free_count.store(other.m_free_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_bitmap       = other.m_bitmap;
    m_bitmap_words = other.m_bitmap_words;
    other.m_memory = nullptr;
    return *this;
}

// ─── init ─────────────────────────────────────────────────────────────────────

void pool_view::init_from_region(void* base, size_t block_size, size_t block_count) noexcept
{
    assert(base && block_size > 0 && std::has_single_bit(block_size)
           && block_size >= sizeof(void*) && block_count > 0);
    assert((reinterpret_cast<uintptr_t>(base) % block_size) == 0);

    m_block_size  = block_size;
    m_block_shift = static_cast<size_t>(std::countr_zero(block_size));
    m_blocks_per_chunk      = block_count;
    m_virtual_block_ceiling = block_count;
    m_committed_blocks.store(block_count, std::memory_order_relaxed);
    m_reserved_blocks.store(block_count, std::memory_order_relaxed);
    m_free_count.store(block_count, std::memory_order_relaxed);

    // bitmap embedded at start of region
    size_t bitmap_bytes = bitmap_bytes_for(block_count);
    std::memset(base, 0, bitmap_bytes);
    m_bitmap       = static_cast<palloc_atomic<uint64_t>*>(base);
    m_bitmap_words = (block_count + 63) / 64;

    // mark tail bits as used
    size_t tail = block_count % 64;
    if (tail)
        m_bitmap[m_bitmap_words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);

    // payload after bitmap, aligned
    void*  payload = static_cast<std::byte*>(base) + bitmap_bytes;
    size_t rem     = required_region_size(block_size, block_count) - bitmap_bytes;
    void*  aligned = std::align(block_size, block_size * block_count, payload, rem);
    assert(aligned);
    m_memory = static_cast<std::byte*>(aligned);
}

void pool_view::init_from_region(void*                    payload,
                                  palloc_atomic<uint64_t>* bitmap,
                                  size_t                   block_size,
                                  size_t                   committed_blocks,
                                  size_t                   virtual_block_ceiling,
                                  size_t                   blocks_per_chunk) noexcept
{
    assert(payload && bitmap && block_size > 0 && std::has_single_bit(block_size)
           && block_size >= sizeof(void*) && committed_blocks > 0
           && virtual_block_ceiling >= committed_blocks);
    assert((reinterpret_cast<uintptr_t>(payload) % block_size) == 0);

    m_block_size  = block_size;
    m_block_shift = static_cast<size_t>(std::countr_zero(block_size));
    m_blocks_per_chunk      = blocks_per_chunk;
    m_virtual_block_ceiling = virtual_block_ceiling;
    m_committed_blocks.store(committed_blocks, std::memory_order_relaxed);
    m_reserved_blocks.store(committed_blocks, std::memory_order_relaxed);
    m_free_count.store(committed_blocks, std::memory_order_relaxed);

    m_bitmap       = bitmap;
    m_bitmap_words = (virtual_block_ceiling + 63) / 64;
    m_memory       = static_cast<std::byte*>(payload);
}

// ─── alloc / free ─────────────────────────────────────────────────────────────

void* pool_view::alloc() noexcept
{
    size_t committed = m_committed_blocks.load(std::memory_order_acquire);
    size_t num_words = (committed + 63) / 64;

    // thread-local hint seeded from pool address + thread id — spreads threads across bitmap
    thread_local size_t tl_hint = 0;
    thread_local const pool_view* tl_pool = nullptr;
    if (tl_pool != this) [[unlikely]]
    {
        tl_pool  = this;
        tl_hint  = (reinterpret_cast<uintptr_t>(this) ^
                    std::hash<std::thread::id>{}(std::this_thread::get_id())) % 64;
    }
    if (tl_hint >= num_words)
        tl_hint = 0;

    size_t slot = bitmap_alloc_bit(m_bitmap, num_words, committed, tl_hint);
    if (slot == static_cast<size_t>(-1))
        return nullptr;

    m_free_count.fetch_sub(1, std::memory_order_relaxed);
    return m_memory + (slot << m_block_shift);
}

size_t pool_view::alloc_batch(size_t count, void* out[]) noexcept
{
    size_t slots[128];
    size_t n = count > 128 ? 128 : count;

    size_t committed = m_committed_blocks.load(std::memory_order_acquire);
    size_t num_words = (committed + 63) / 64;

    // same thread-local hint as alloc() — keeps batch and single alloc in the same region
    thread_local size_t tl_hint = 0;
    thread_local const pool_view* tl_pool = nullptr;
    if (tl_pool != this) [[unlikely]]
    {
        tl_pool = this;
        tl_hint = (reinterpret_cast<uintptr_t>(this) ^
                   std::hash<std::thread::id>{}(std::this_thread::get_id())) % 64;
    }
    if (tl_hint >= num_words)
        tl_hint = 0;

    size_t found = bitmap_alloc_batch(m_bitmap, num_words, committed, n, slots, tl_hint);

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
    bitmap_free_bit(m_bitmap, slot);
    m_free_count.fetch_add(1, std::memory_order_relaxed);
}

void pool_view::free_batch(std::span<void*> ptrs) noexcept
{
    size_t count = 0;
    for (void* ptr : ptrs)
    {
        if (!ptr)
            continue;
        assert(owns(ptr));
        size_t slot = static_cast<size_t>(static_cast<std::byte*>(ptr) - m_memory) >> m_block_shift;
        bitmap_free_bit(m_bitmap, slot);
        ++count;
    }
    m_free_count.fetch_add(count, std::memory_order_relaxed);
}

void pool_view::reset() noexcept
{
    size_t committed = m_committed_blocks.load(std::memory_order_relaxed);
    size_t num_words = (committed + 63) / 64;

    std::memset(m_bitmap, 0, num_words * sizeof(uint64_t));

    // mark tail bits of last committed word as used
    size_t tail = committed % 64;
    if (tail)
        m_bitmap[num_words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);

    m_free_count.store(committed, std::memory_order_relaxed);
}

// ─── growth ───────────────────────────────────────────────────────────────────

bool pool_view::try_reserve_chunk(size_t& old_committed) noexcept
{
    old_committed = m_committed_blocks.load(std::memory_order_acquire);
    size_t next   = old_committed + m_blocks_per_chunk;

    if (next > m_virtual_block_ceiling)
        return false;

    size_t expected = old_committed;
    return m_reserved_blocks.compare_exchange_strong(expected, next,
               std::memory_order_acquire, std::memory_order_relaxed);
}

void pool_view::advance_committed(size_t new_committed) noexcept
{
    size_t added = new_committed - m_committed_blocks.load(std::memory_order_relaxed);
    m_free_count.fetch_add(added, std::memory_order_relaxed);
    m_committed_blocks.store(new_committed, std::memory_order_release);
}

// ─── shrink helpers ───────────────────────────────────────────────────────────

bool pool_view::is_chunk_empty(size_t chunk_idx) const noexcept
{
    size_t word_start = (chunk_idx * m_blocks_per_chunk) / 64;
    size_t word_end   = ((chunk_idx + 1) * m_blocks_per_chunk + 63) / 64;
    if (word_end > m_bitmap_words)
        word_end = m_bitmap_words;
    return bitmap_is_word_range_empty(m_bitmap, word_start, word_end);
}

void pool_view::decommit_blocks(size_t new_committed) noexcept
{
    size_t old = m_committed_blocks.load(std::memory_order_relaxed);
    if (old > new_committed)
        m_free_count.fetch_sub(old - new_committed, std::memory_order_relaxed);
    m_reserved_blocks.store(new_committed, std::memory_order_relaxed);
    m_committed_blocks.store(new_committed, std::memory_order_relaxed);
}

// ─── accessors ────────────────────────────────────────────────────────────────

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
    auto   p         = static_cast<const std::byte*>(ptr);
    size_t committed = m_committed_blocks.load(std::memory_order_relaxed);
    if (p < m_memory || p >= m_memory + m_block_size * committed)
        return false;
    return (static_cast<size_t>(p - m_memory) & (m_block_size - 1)) == 0;
}

} // namespace AL
