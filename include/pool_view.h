#pragma once

#include "palloc_atomic.h"
#include "profiler.h"
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <thread>

namespace AL
{

// alloc one bit from [words, words+num_words), scanning only up to num_slots.
// tl_hint is a thread-local starting word that advances to spread threads across the bitmap.
// returns slot index or -1 if full.
template<bool Tthreaded>
static size_t bm_alloc_bit(palloc_atomic<uint64_t, Tthreaded>* words, size_t num_words, size_t num_slots, size_t& tl_hint) noexcept
{
    for (int pass = 0; pass < 2; ++pass)
    {
        size_t start = (pass == 0) ? tl_hint : 0;
        size_t stop = (pass == 0) ? num_words : tl_hint;

        if (start >= num_words)
            stop = 0;

        for (size_t w = start; w < (pass == 0 ? num_words : stop); ++w)
        {
            uint64_t word = words[w].load(std::memory_order_relaxed);

            while (word != ~uint64_t(0))
            {
                size_t bit = static_cast<size_t>(std::countr_zero(~word));
                size_t slot = w * 64 + bit;

                if (slot >= num_slots)
                    return static_cast<size_t>(-1);

                uint64_t mask = uint64_t(1) << bit;
                uint64_t old = words[w].fetch_or(mask, std::memory_order_acquire);

                if (!(old & mask))
                {
                    // we claimed the bit
                    if ((old | mask) == ~uint64_t(0))
                        tl_hint = w + 1;
                    return slot;
                }

                // another thread took this bit; re-read and try the next free bit
                word = old | mask;
            }
        }
    }
    return static_cast<size_t>(-1);
}

// attempt to claim up to `needed` free bits from words[w] via CAS retry.
// `already_found` is the current output count, used to guard the num_slots boundary.
// returns the claimed mask (0 if nothing claimed); `word` is updated on CAS failure.
template<bool Tthreaded>
static uint64_t claim_bits_in_word(
    palloc_atomic<uint64_t, Tthreaded>* words, size_t w, uint64_t& word, size_t num_slots, size_t needed, size_t already_found) noexcept
{
    // lambda
    auto compute_claim = [&](uint64_t cur) -> std::pair<uint64_t, uint64_t> {
        if (cur == ~uint64_t(0))
            return {0, 0};
        uint64_t free_bits = ~cur, claimed = 0;
        size_t tmp = already_found;
        while (free_bits && tmp < already_found + needed)
        {
            size_t bit = static_cast<size_t>(std::countr_zero(free_bits));
            if (w * 64 + bit >= num_slots)
                break;
            claimed |= uint64_t(1) << bit;
            free_bits &= free_bits - 1;
            ++tmp;
        }
        return {claimed, cur | claimed};
    };

    if constexpr (!Tthreaded)
    {
        auto [claimed, new_word] = compute_claim(word);

        if (claimed)
            words[w].store(new_word, std::memory_order_relaxed);

        return claimed;
    }
    else
    {
        uint64_t claimed = 0, new_word = 0;
        do
        {
            auto [c, nw] = compute_claim(word);
            claimed = c;
            new_word = nw;

            if (!claimed)
                break;
        }
        while (!words[w].compare_exchange_weak(word, new_word, std::memory_order_acquire, std::memory_order_relaxed));
        return claimed;
    }
}

// write slot indices for each set bit in `mask` (word index `w`) into out[found..].
// returns the new found count.
static size_t emit_slots(size_t w, uint64_t mask, size_t* out, size_t found) noexcept
{
    while (mask)
    {
        size_t bit = static_cast<size_t>(std::countr_zero(mask));
        out[found++] = w * 64 + bit;
        mask &= mask - 1;
    }
    return found;
}

// two-pass scan starting from tl_hint
template<bool Tthreaded>
static size_t bm_alloc_batch(
    palloc_atomic<uint64_t, Tthreaded>* words, size_t num_words, size_t num_slots, size_t count, size_t out[], size_t& tl_hint) noexcept
{
    size_t found = 0;

    for (int pass = 0; pass < 2 && found < count; ++pass)
    {
        size_t start = (pass == 0) ? tl_hint : 0;
        size_t stop = (pass == 0) ? num_words : tl_hint;

        if (start >= num_words)
            stop = 0;

        for (size_t w = start; w < (pass == 0 ? num_words : stop) && found < count; ++w)
        {
            uint64_t word = words[w].load(std::memory_order_relaxed);
            uint64_t claimed = claim_bits_in_word<Tthreaded>(words, w, word, num_slots, count - found, found);

            if (!claimed)
                continue;

            if ((word | claimed) == ~uint64_t(0))
                tl_hint = w + 1;

            found = emit_slots(w, claimed, out, found);
        }
    }
    return found;
}

template<bool Tthreaded>
static void bm_free_bit(palloc_atomic<uint64_t, Tthreaded>* words, size_t slot) noexcept
{
    words[slot >> 6].fetch_and(~(uint64_t(1) << (slot & 63)), std::memory_order_release);
}

template<bool Tthreaded>
static bool bm_is_range_empty(const palloc_atomic<uint64_t, Tthreaded>* words, size_t word_start, size_t word_end) noexcept
{
    for (size_t w = word_start; w < word_end; ++w)
        if (words[w].load(std::memory_order_relaxed) != 0)
            return false;
    return true;
}

// pool_view: flat bitmap allocator over a contiguous memory region.
//
// The bitmap covers all blocks up to virtual_block_ceiling, committed upfront.
// The payload region is virtual_alloc'd (reserved) and committed in chunks on
// demand by the owning slab. alloc/free only scan up to committed_blocks words,
// so uncommitted payload is never touched.
//
// Tthreaded=true  — all internal atomics use std::atomic (lock-free, thread-safe)
// Tthreaded=false — all internal atomics use plain loads/stores (single-threaded, zero overhead)
template<bool Tthreaded = PALLOC_THREADED_DEFAULT>
class alignas(64) pool_view
{
public:
    pool_view() noexcept = default;

    pool_view(const pool_view&) = delete;
    pool_view& operator=(const pool_view&) = delete;

    pool_view(pool_view&& other) noexcept
        : m_memory(other.m_memory), m_block_shift(other.m_block_shift), m_committed_blocks(other.m_committed_blocks.load(std::memory_order_relaxed)),
          m_free_count(other.m_free_count.load(std::memory_order_relaxed)), m_bitmap(other.m_bitmap), m_block_size(other.m_block_size),
          m_blocks_per_chunk(other.m_blocks_per_chunk), m_reserved_blocks(other.m_reserved_blocks.load(std::memory_order_relaxed)),
          m_virtual_block_ceiling(other.m_virtual_block_ceiling), m_bitmap_words(other.m_bitmap_words)
    {
        other.m_memory = nullptr;
    }

    pool_view& operator=(pool_view&& other) noexcept
    {
        if (this == &other)
            return *this;
        m_memory = other.m_memory;
        m_block_shift = other.m_block_shift;
        m_committed_blocks.store(other.m_committed_blocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
        m_free_count.store(other.m_free_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
        m_bitmap = other.m_bitmap;
        m_block_size = other.m_block_size;
        m_blocks_per_chunk = other.m_blocks_per_chunk;
        m_reserved_blocks.store(other.m_reserved_blocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
        m_virtual_block_ceiling = other.m_virtual_block_ceiling;
        m_bitmap_words = other.m_bitmap_words;
        other.m_memory = nullptr;
        return *this;
    }

    // standalone path: bitmap embedded at start of region, fixed capacity
    void init_from_region(void* base, size_t block_size, size_t block_count) noexcept
    {
        assert(base && block_size > 0 && std::has_single_bit(block_size) && block_size >= sizeof(void*) && block_count > 0);
        assert((reinterpret_cast<uintptr_t>(base) % block_size) == 0);

        m_block_size = block_size;
        m_block_shift = static_cast<size_t>(std::countr_zero(block_size));
        m_blocks_per_chunk = block_count;
        m_virtual_block_ceiling = block_count;
        m_committed_blocks.store(block_count, std::memory_order_relaxed);
        m_reserved_blocks.store(block_count, std::memory_order_relaxed);
        m_free_count.store(block_count, std::memory_order_relaxed);

        size_t bitmap_bytes = bitmap_bytes_for(block_count);
        std::memset(base, 0, bitmap_bytes);
        // bitmap embedded at start of region
        m_bitmap = static_cast<palloc_atomic<uint64_t, Tthreaded>*>(base);
        m_bitmap_words = (block_count + 63) / 64;

        // mark tail bits as used
        size_t tail = block_count % 64;
        if (tail)
            m_bitmap[m_bitmap_words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);

        // payload after bitmap, aligned
        void* payload = static_cast<std::byte*>(base) + bitmap_bytes;
        size_t rem = required_region_size(block_size, block_count) - bitmap_bytes;
        void* aligned = std::align(block_size, block_size * block_count, payload, rem);
        assert(aligned);
        m_memory = static_cast<std::byte*>(aligned);
    }

    // growable path: flat bitmap pre-allocated externally, payload lazy-committed
    // bitmap must cover virtual_block_ceiling blocks and be zeroed by caller
    void init_from_region(void* payload,
                          palloc_atomic<uint64_t, Tthreaded>* bitmap,
                          size_t block_size,
                          size_t committed_blocks,
                          size_t virtual_block_ceiling,
                          size_t blocks_per_chunk) noexcept
    {
        assert(payload && bitmap && block_size > 0 && std::has_single_bit(block_size) && block_size >= sizeof(void*) && committed_blocks > 0 &&
               virtual_block_ceiling >= committed_blocks);
        assert((reinterpret_cast<uintptr_t>(payload) % block_size) == 0);

        m_block_size = block_size;
        m_block_shift = static_cast<size_t>(std::countr_zero(block_size));
        m_blocks_per_chunk = blocks_per_chunk;
        m_virtual_block_ceiling = virtual_block_ceiling;
        m_committed_blocks.store(committed_blocks, std::memory_order_relaxed);
        m_reserved_blocks.store(committed_blocks, std::memory_order_relaxed);
        m_free_count.store(committed_blocks, std::memory_order_relaxed);
        m_bitmap = bitmap;
        m_bitmap_words = (virtual_block_ceiling + 63) / 64;
        m_memory = static_cast<std::byte*>(payload);
    }

    [[nodiscard]] void* alloc() noexcept
    {
        PALLOC_ZONE("pool_view::alloc");
        size_t committed = m_committed_blocks.load(std::memory_order_acquire);
        size_t num_words = (committed + 63) / 64;

        thread_local size_t tl_hint = 0;
        thread_local const pool_view* tl_pool = nullptr;
        // thread-local hint seeded from pool address + thread id — spreads threads across bitmap
        if (tl_pool != this) [[unlikely]]
        {
            tl_pool = this;
            tl_hint = (reinterpret_cast<uintptr_t>(this) ^ std::hash<std::thread::id>{}(std::this_thread::get_id())) % 64;
        }
        if (tl_hint >= num_words)
            tl_hint = 0;

        size_t slot = bm_alloc_bit<Tthreaded>(m_bitmap, num_words, committed, tl_hint);
        if (slot == static_cast<size_t>(-1))
            return nullptr;

        m_free_count.fetch_sub(1, std::memory_order_relaxed);
        return m_memory + (slot << m_block_shift);
    }

    [[nodiscard]] void* calloc() noexcept
    {
        void* ptr = alloc();
        if (ptr)
            std::memset(ptr, 0, m_block_size);
        return ptr;
    }

    [[nodiscard]] size_t alloc_batch(size_t count, void* out[]) noexcept
    {
        PALLOC_ZONE("pool_view::alloc_batch");
        size_t slots[128];
        size_t n = count > 128 ? 128 : count;

        size_t committed = m_committed_blocks.load(std::memory_order_acquire);
        size_t num_words = (committed + 63) / 64;

        // same thread-local hint as alloc() — keeps batch and single alloc in the same region
        thread_local size_t tl_hint = 0;
        thread_local const pool_view* tl_pool = nullptr;
        // same thread-local hint as alloc() — keeps batch and single alloc in the same region
        if (tl_pool != this) [[unlikely]]
        {
            tl_pool = this;
            tl_hint = (reinterpret_cast<uintptr_t>(this) ^ std::hash<std::thread::id>{}(std::this_thread::get_id())) % 64;
        }
        if (tl_hint >= num_words)
            tl_hint = 0;

        size_t found = bm_alloc_batch<Tthreaded>(m_bitmap, num_words, committed, n, slots, tl_hint);
        m_free_count.fetch_sub(found, std::memory_order_relaxed);
        for (size_t i = 0; i < found; ++i)
            out[i] = m_memory + (slots[i] << m_block_shift);
        return found;
    }

    void free(void* ptr) noexcept
    {
        if (!ptr)
            return;
        assert(owns(ptr));
        size_t slot = static_cast<size_t>(static_cast<std::byte*>(ptr) - m_memory) >> m_block_shift;
        bm_free_bit<Tthreaded>(m_bitmap, slot);
        m_free_count.fetch_add(1, std::memory_order_relaxed);
    }

    void free_batch(std::span<void*> ptrs) noexcept
    {
        PALLOC_ZONE("pool_view::free_batch");
        size_t count = 0;
        for (void* ptr : ptrs)
        {
            if (!ptr)
                continue;
            assert(owns(ptr));
            size_t slot = static_cast<size_t>(static_cast<std::byte*>(ptr) - m_memory) >> m_block_shift;
            bm_free_bit<Tthreaded>(m_bitmap, slot);
            ++count;
        }
        m_free_count.fetch_add(count, std::memory_order_relaxed);
    }

    void reset() noexcept
    {
        PALLOC_ZONE("pool_view::reset");
        size_t committed = m_committed_blocks.load(std::memory_order_relaxed);
        size_t num_words = (committed + 63) / 64;
        std::memset(m_bitmap, 0, num_words * sizeof(uint64_t));
        // mark tail bits of last committed word as used
        size_t tail = committed % 64;
        if (tail)
            m_bitmap[num_words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);
        m_free_count.store(committed, std::memory_order_relaxed);
    }

    // growth interface
    [[nodiscard]] bool try_reserve_chunk(size_t& old_committed) noexcept
    {
        old_committed = m_committed_blocks.load(std::memory_order_acquire);
        size_t next = old_committed + m_blocks_per_chunk;
        if (next > m_virtual_block_ceiling)
            return false;
        if constexpr (!Tthreaded)
        {
            m_reserved_blocks.store(next, std::memory_order_relaxed);
            return true;
        }
        else
        {
            size_t expected = old_committed;
            return m_reserved_blocks.compare_exchange_strong(expected, next, std::memory_order_acquire, std::memory_order_relaxed);
        }
    }

    void advance_committed(size_t new_committed) noexcept
    {
        size_t added = new_committed - m_committed_blocks.load(std::memory_order_relaxed);
        m_free_count.fetch_add(added, std::memory_order_relaxed);
        m_committed_blocks.store(new_committed, std::memory_order_release);
    }

    [[nodiscard]] bool is_chunk_empty(size_t chunk_idx) const noexcept
    {
        size_t word_start = (chunk_idx * m_blocks_per_chunk) / 64;
        size_t word_end = ((chunk_idx + 1) * m_blocks_per_chunk + 63) / 64;
        if (word_end > m_bitmap_words)
            word_end = m_bitmap_words;
        return bm_is_range_empty<Tthreaded>(m_bitmap, word_start, word_end);
    }

    void decommit_blocks(size_t new_committed) noexcept
    {
        size_t old = m_committed_blocks.load(std::memory_order_relaxed);
        if (old > new_committed)
            m_free_count.fetch_sub(old - new_committed, std::memory_order_relaxed);
        m_reserved_blocks.store(new_committed, std::memory_order_relaxed);
        m_committed_blocks.store(new_committed, std::memory_order_relaxed);
    }

    [[nodiscard]] size_t free_count() const noexcept
    {
        return m_free_count.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t block_count() const noexcept
    {
        return m_committed_blocks.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t block_size() const noexcept
    {
        return m_block_size;
    }

    [[nodiscard]] size_t capacity() const noexcept
    {
        return m_block_size * m_committed_blocks.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t committed_blocks() const noexcept
    {
        return m_committed_blocks.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t virtual_block_ceiling() const noexcept
    {
        return m_virtual_block_ceiling;
    }

    [[nodiscard]] size_t blocks_per_chunk() const noexcept
    {
        return m_blocks_per_chunk;
    }

    [[nodiscard]] bool is_initialized() const noexcept
    {
        return m_memory != nullptr;
    }

    [[nodiscard]] std::byte* memory_start() const noexcept
    {
        return m_memory;
    }

    [[nodiscard]] std::byte* memory_end() const noexcept
    {
        if (!m_memory)
            return nullptr;
        return m_memory + m_block_size * m_committed_blocks.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool owns(const void* ptr) const noexcept
    {
        if (!ptr || !m_memory)
            return false;
        auto p = static_cast<const std::byte*>(ptr);
        size_t committed = m_committed_blocks.load(std::memory_order_relaxed);
        if (p < m_memory || p >= m_memory + m_block_size * committed)
            return false;
        return (static_cast<size_t>(p - m_memory) & (m_block_size - 1)) == 0;
    }

    [[nodiscard]] static size_t required_region_size(size_t block_size, size_t block_count) noexcept
    {
        size_t bitmap_bytes = ((block_count + 63) / 64) * sizeof(uint64_t);
        size_t aligned_offset = ((bitmap_bytes + block_size - 1) / block_size) * block_size;
        return aligned_offset + block_size * block_count;
    }

    [[nodiscard]] static size_t bitmap_bytes_for(size_t block_count) noexcept
    {
        return ((block_count + 63) / 64) * sizeof(uint64_t);
    }

private:
    // hot fields on alloc/free path — all fit within cache line 0 (offsets 0–63)
    std::byte* m_memory = nullptr;                          // offset 0
    size_t m_block_shift = 0;                               // offset 8
    palloc_atomic<size_t, Tthreaded> m_committed_blocks{0}; // offset 16
    palloc_atomic<size_t, Tthreaded> m_free_count{0};       // offset 24
    palloc_atomic<uint64_t, Tthreaded>* m_bitmap = nullptr; // offset 32

    // cold fields follow
    size_t m_block_size = 0;
    size_t m_blocks_per_chunk = 0;
    palloc_atomic<size_t, Tthreaded> m_reserved_blocks{0};
    size_t m_virtual_block_ceiling = 0;
    size_t m_bitmap_words = 0;
};

} // namespace AL
