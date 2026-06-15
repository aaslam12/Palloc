#pragma once

#include "palloc_atomic.h"
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace AL
{

// flat atomic bitmap over an external memory region
// non-owning: does not allocate or free memory
// 0 = free, 1 = allocated
template<bool Tthreaded = PALLOC_THREADED_DEFAULT>
class bitmap
{
public:
    bitmap() noexcept = default;

    bitmap(const bitmap&) = delete;
    bitmap& operator=(const bitmap&) = delete;

    bitmap(bitmap&& other) noexcept
        : m_words(other.m_words), m_num_words(other.m_num_words), m_num_slots(other.m_num_slots),
          m_free_count(other.m_free_count.load(std::memory_order_relaxed)), m_hint(other.m_hint.load(std::memory_order_relaxed))
    {
        other.m_words = nullptr;
    }

    bitmap& operator=(bitmap&& other) noexcept
    {
        if (this == &other)
            return *this;
        m_words = other.m_words;
        m_num_words = other.m_num_words;
        m_num_slots = other.m_num_slots;
        m_free_count.store(other.m_free_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
        m_hint.store(other.m_hint.load(std::memory_order_relaxed), std::memory_order_relaxed);
        other.m_words = nullptr;
        return *this;
    }

    // base must be pre-zeroed and large enough for required_size(num_slots) bytes
    // trailing bits beyond num_slots are pre-set to 1 so scans terminate correctly
    void init(void* base, size_t num_slots) noexcept
    {
        assert(base != nullptr);
        assert(num_slots > 0);
        m_words = static_cast<palloc_atomic<uint64_t, Tthreaded>*>(base);
        m_num_slots = num_slots;
        m_num_words = (num_slots + 63) / 64;
        m_free_count.store(num_slots, std::memory_order_relaxed);
        m_hint.store(0, std::memory_order_relaxed);
        size_t tail = num_slots % 64;
        if (tail != 0)
            m_words[m_num_words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);
    }

    // atomically claim one free slot - returns slot index or (size_t)-1 if full
    [[nodiscard]] size_t alloc_bit() noexcept
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            size_t hint = m_hint.load(std::memory_order_relaxed);
            size_t start = (pass == 0) ? hint : 0;
            size_t stop = (pass == 0) ? m_num_words : hint;
            if (start >= m_num_words)
                stop = 0;

            for (size_t w = start; w < (pass == 0 ? m_num_words : stop); ++w)
            {
                uint64_t word = m_words[w].load(std::memory_order_relaxed);
                while (word != ~uint64_t(0))
                {
                    size_t bit = static_cast<size_t>(std::countr_zero(~word));
                    size_t slot = w * 64 + bit;
                    if (slot >= m_num_slots)
                        return static_cast<size_t>(-1);

                    uint64_t new_word = word | (uint64_t(1) << bit);
                    if constexpr (!Tthreaded)
                    {
                        m_words[w].store(new_word, std::memory_order_relaxed);
                        m_free_count.fetch_sub(1, std::memory_order_relaxed);
                        if (new_word == ~uint64_t(0))
                            m_hint.store(w + 1, std::memory_order_relaxed);
                        return slot;
                    }
                    else if (m_words[w].compare_exchange_weak(word, new_word, std::memory_order_acquire, std::memory_order_relaxed))
                    {
                        m_free_count.fetch_sub(1, std::memory_order_relaxed);
                        if (new_word == ~uint64_t(0))
                            m_hint.store(w + 1, std::memory_order_relaxed);
                        return slot;
                    }
                }
            }
        }
        return static_cast<size_t>(-1);
    }

    // atomically claim up to count free slots - writes indices into out[], returns count claimed
    size_t alloc_bits_batch(size_t count, size_t out[]) noexcept
    {
        if (count == 0)
            return 0;
        size_t found = 0;
        for (size_t w = 0; w < m_num_words && found < count; ++w)
        {
            uint64_t word = m_words[w].load(std::memory_order_relaxed);
            if (word == ~uint64_t(0))
                continue;

            uint64_t claimed = 0;
            size_t local_found = 0;
            uint64_t new_word = 0;
            auto compute_batch = [&]()
            {
                claimed = 0; local_found = 0;
                uint64_t free_bits = ~word;
                size_t tmp = found;
                while (free_bits && tmp < count)
                {
                    size_t bit = static_cast<size_t>(std::countr_zero(free_bits));
                    size_t slot = w * 64 + bit;
                    if (slot >= m_num_slots) { free_bits = 0; break; }
                    claimed |= (uint64_t(1) << bit);
                    free_bits &= free_bits - 1;
                    ++tmp; ++local_found;
                }
                new_word = word | claimed;
            };
            if constexpr (!Tthreaded)
            {
                compute_batch();
                if (claimed) m_words[w].store(new_word, std::memory_order_relaxed);
            }
            else
            {
                do { compute_batch(); if (!claimed) break; }
                while (!m_words[w].compare_exchange_weak(word, new_word, std::memory_order_acquire, std::memory_order_relaxed));
            }

            if (claimed == 0)
                continue;
            uint64_t bits = claimed;
            while (bits)
            {
                size_t bit = static_cast<size_t>(std::countr_zero(bits));
                out[found++] = w * 64 + bit;
                bits &= bits - 1;
            }
            m_free_count.fetch_sub(local_found, std::memory_order_relaxed);
            if (new_word == ~uint64_t(0))
                m_hint.store(w + 1, std::memory_order_relaxed);
        }
        return found;
    }

    // like alloc_bit but scans only up to limit slots
    [[nodiscard]] size_t alloc_bit(size_t limit) noexcept
    {
        if (limit == 0)
            return static_cast<size_t>(-1);
        size_t limit_slots = limit < m_num_slots ? limit : m_num_slots;
        size_t limit_words = (limit_slots + 63) / 64;
        if (limit_words == 0)
            return static_cast<size_t>(-1);
        size_t limit_tail = limit_slots % 64;
        uint64_t tail_mask = (limit_tail != 0) ? ~((uint64_t(1) << limit_tail) - 1) : 0;

        for (size_t pass = 0; pass < 2; ++pass)
        {
            size_t hint = m_hint.load(std::memory_order_relaxed);
            size_t start = (pass == 0) ? hint : 0;
            size_t stop = (pass == 0) ? limit_words : hint;
            if (start >= limit_words)
                stop = 0;

            for (size_t w = start; w < (pass == 0 ? limit_words : stop); ++w)
            {
                uint64_t word = m_words[w].load(std::memory_order_relaxed);
                while (true)
                {
                    uint64_t effective = (w == limit_words - 1) ? (word | tail_mask) : word;
                    if (effective == ~uint64_t(0))
                        break;
                    size_t bit = static_cast<size_t>(std::countr_zero(~effective));
                    size_t slot = w * 64 + bit;
                    if (slot >= limit_slots)
                        break;
                    uint64_t mask = uint64_t(1) << bit;
                    uint64_t old = m_words[w].fetch_or(mask, std::memory_order_acquire);
                    if (!(old & mask))
                    {
                        // we claimed the bit
                        m_free_count.fetch_sub(1, std::memory_order_relaxed);
                        if (((old | mask) | tail_mask) == ~uint64_t(0))
                            m_hint.store(w + 1, std::memory_order_relaxed);
                        return slot;
                    }
                    // another thread took this bit; update word and try the next free bit
                    word = old | mask;
                }
            }
        }
        return static_cast<size_t>(-1);
    }

    // like alloc_bits_batch but scans only up to limit slots
    size_t alloc_bits_batch(size_t count, size_t limit, size_t out[]) noexcept
    {
        if (count == 0 || limit == 0)
            return 0;
        size_t limit_slots = limit < m_num_slots ? limit : m_num_slots;
        size_t limit_words = (limit_slots + 63) / 64;
        if (limit_words == 0)
            return 0;
        size_t limit_tail = limit_slots % 64;
        uint64_t tail_mask = (limit_tail != 0) ? ~((uint64_t(1) << limit_tail) - 1) : 0;

        size_t found = 0;
        for (int pass = 0; pass < 2 && found < count; ++pass)
        {
            size_t hint = m_hint.load(std::memory_order_relaxed);
            size_t start = (pass == 0) ? hint : 0;
            size_t stop = (pass == 0) ? limit_words : hint;
            if (start >= limit_words)
                stop = 0;

            for (size_t w = start; w < (pass == 0 ? limit_words : stop) && found < count; ++w)
            {
                uint64_t word = m_words[w].load(std::memory_order_relaxed);
                uint64_t claimed = 0;
                size_t local_found = 0;
                uint64_t new_word = 0;
                auto compute_batch_lim = [&]()
                {
                    claimed = 0; local_found = 0;
                    uint64_t effective = (w == limit_words - 1) ? (word | tail_mask) : word;
                    if (effective == ~uint64_t(0)) return;
                    uint64_t free_bits = ~effective;
                    size_t tmp = found;
                    while (free_bits && tmp < count)
                    {
                        size_t bit = static_cast<size_t>(std::countr_zero(free_bits));
                        size_t slot = w * 64 + bit;
                        if (slot >= limit_slots) { free_bits = 0; break; }
                        claimed |= uint64_t(1) << bit;
                        free_bits &= free_bits - 1;
                        ++tmp; ++local_found;
                    }
                    new_word = word | claimed;
                };
                if constexpr (!Tthreaded)
                {
                    compute_batch_lim();
                    if (claimed) m_words[w].store(new_word, std::memory_order_relaxed);
                }
                else
                {
                    do { compute_batch_lim(); if (!claimed) break; }
                    while (!m_words[w].compare_exchange_weak(word, new_word, std::memory_order_acquire, std::memory_order_relaxed));
                }

                if (claimed == 0)
                    continue;
                uint64_t bits = claimed;
                while (bits)
                {
                    size_t bit = static_cast<size_t>(std::countr_zero(bits));
                    out[found++] = w * 64 + bit;
                    bits &= bits - 1;
                }
                m_free_count.fetch_sub(local_found, std::memory_order_relaxed);
                if ((new_word | tail_mask) == ~uint64_t(0))
                    m_hint.store(w + 1, std::memory_order_relaxed);
            }
        }
        return found;
    }

    // atomically free a slot
    void free_bit(size_t slot) noexcept
    {
        assert(slot < m_num_slots);
        size_t word_idx = slot >> 6;
        uint64_t mask = uint64_t(1) << (slot & 63);
        uint64_t prev = m_words[word_idx].fetch_and(~mask, std::memory_order_release);
        assert((prev & mask) != 0 && "double free");
        (void)prev;
        m_free_count.fetch_add(1, std::memory_order_relaxed);
    }

    // batch free - accumulates masks per word, one fetch_and per touched word (slots must be sorted by word index)
    void free_bits_batch(const size_t slots[], size_t count) noexcept
    {
        size_t i = 0;
        while (i < count)
        {
            size_t word_idx = slots[i] >> 6;
            uint64_t mask = uint64_t(1) << (slots[i] & 63);
            size_t j = i + 1;
            while (j < count && (slots[j] >> 6) == word_idx)
                mask |= uint64_t(1) << (slots[j++] & 63);
            m_words[word_idx].fetch_and(~mask, std::memory_order_release);
            i = j;
        }
        m_free_count.fetch_add(count, std::memory_order_relaxed);
    }

    // reset all slots to free - not thread-safe
    void reset() noexcept
    {
        std::memset(m_words, 0, m_num_words * sizeof(uint64_t));
        size_t tail = m_num_slots % 64;
        if (tail != 0)
            m_words[m_num_words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);
        m_free_count.store(m_num_slots, std::memory_order_relaxed);
        m_hint.store(0, std::memory_order_relaxed);
    }

    // reset only the first active_words words - not thread-safe
    void reset_to(size_t active_words, size_t active_slots) noexcept
    {
        if (active_words == 0 || active_slots == 0)
            return;
        std::memset(m_words, 0, active_words * sizeof(uint64_t));
        size_t tail = active_slots % 64;
        if (tail != 0)
            m_words[active_words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);
        m_free_count.store(active_slots, std::memory_order_relaxed);
        m_hint.store(0, std::memory_order_relaxed);
    }

    // returns true if every bit in [word_start, word_start + word_count) is 0
    [[nodiscard]] bool is_range_empty(size_t word_start, size_t word_count) const noexcept
    {
        for (size_t w = word_start; w < word_start + word_count; ++w)
            if (m_words[w].load(std::memory_order_relaxed) != 0)
                return false;
        return true;
    }

    [[nodiscard]] bool is_slot_set(size_t slot) const noexcept
    {
        return (m_words[slot >> 6].load(std::memory_order_relaxed) >> (slot & 63)) & 1;
    }

    void set_slot(size_t slot) noexcept
    {
        m_words[slot >> 6].fetch_or(uint64_t(1) << (slot & 63), std::memory_order_relaxed);
    }

    void clear_slot(size_t slot) noexcept
    {
        m_words[slot >> 6].fetch_and(~(uint64_t(1) << (slot & 63)), std::memory_order_relaxed);
    }

    // returns slot index or (size_t)-1 if not
    // performs a linear scan over words
    [[nodiscard]] size_t find_lowest_set_bit() const noexcept
    {
        // scans forward from the beginning — countr_zero compiles to TZCNT (1 cycle), early exit on first non-zero word
        for (size_t w = 0; w < m_num_words; ++w)
        {
            uint64_t word = m_words[w].load(std::memory_order_relaxed);
            if (word != 0)
            {
                size_t slot = w * 64 + static_cast<size_t>(std::countr_zero(word));
                return slot < m_num_slots ? slot : static_cast<size_t>(-1);
            }
        }
        return static_cast<size_t>(-1);
    }

    // returns slot index or (size_t)-1 if not
    // performs a linear scan over words
    [[nodiscard]] size_t find_highest_set_bit() const noexcept
    {
        if (m_num_words == 0)
            return static_cast<size_t>(-1);
        // mask out pre-set tail bits in the last word before scanning
        size_t tail = m_num_slots % 64;
        uint64_t last = m_words[m_num_words - 1].load(std::memory_order_relaxed);
        if (tail != 0)
            last &= (uint64_t(1) << tail) - 1;
        if (last != 0)
            return (m_num_words - 1) * 64 + (63 - static_cast<size_t>(std::countl_zero(last)));
        for (size_t i = m_num_words - 1; i > 0; --i)
        {
            uint64_t word = m_words[i - 1].load(std::memory_order_relaxed);
            if (word != 0)
                return (i - 1) * 64 + (63 - static_cast<size_t>(std::countl_zero(word)));
        }
        return static_cast<size_t>(-1);
    }

    [[nodiscard]] size_t free_count() const noexcept
    {
        return m_free_count.load(std::memory_order_relaxed);
    }
    void set_free_count(size_t n) noexcept
    {
        m_free_count.store(n, std::memory_order_relaxed);
    }
    void fetch_add_free_count(size_t n) noexcept
    {
        m_free_count.fetch_add(n, std::memory_order_relaxed);
    }
    [[nodiscard]] size_t num_slots() const noexcept
    {
        return m_num_slots;
    }
    [[nodiscard]] size_t num_words() const noexcept
    {
        return m_num_words;
    }
    [[nodiscard]] bool is_init() const noexcept
    {
        return m_words != nullptr;
    }

    [[nodiscard]] static constexpr size_t required_size(size_t num_slots) noexcept
    {
        return ((num_slots + 63) / 64) * sizeof(uint64_t);
    }

private:
    palloc_atomic<uint64_t, Tthreaded>* m_words = nullptr;
    size_t m_num_words = 0;
    size_t m_num_slots = 0;
    palloc_atomic<size_t, Tthreaded> m_free_count{0};
    palloc_atomic<size_t, Tthreaded> m_hint{0};
};

} // namespace AL
