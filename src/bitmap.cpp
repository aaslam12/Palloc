#include "bitmap.h"
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstring>

namespace AL
{

bitmap::bitmap(bitmap&& other) noexcept
    : m_words(other.m_words), m_num_words(other.m_num_words), m_num_slots(other.m_num_slots),
      m_free_count(other.m_free_count.load(std::memory_order_relaxed)), m_hint(other.m_hint.load(std::memory_order_relaxed))
{
    other.m_words = nullptr;
}

bitmap& bitmap::operator=(bitmap&& other) noexcept
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

void bitmap::init(void* base, size_t num_slots) noexcept
{
    assert(base != nullptr);
    assert(num_slots > 0);

    m_words = static_cast<palloc_atomic<uint64_t>*>(base);
    m_num_slots = num_slots;
    m_num_words = (num_slots + 63) / 64;
    m_free_count.store(num_slots, std::memory_order_relaxed);
    m_hint.store(0, std::memory_order_relaxed);

    size_t tail = num_slots % 64;
    if (tail != 0)
        m_words[m_num_words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);
}

size_t bitmap::alloc_bit() noexcept
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
                if (m_words[w].compare_exchange_weak(word, new_word, std::memory_order_acquire, std::memory_order_relaxed))
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

size_t bitmap::alloc_bits_batch(size_t count, size_t out[]) noexcept
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

        do
        {
            claimed = 0;
            local_found = 0;
            uint64_t free_bits = ~word;
            size_t tmp = found;

            while (free_bits && tmp < count)
            {
                size_t bit = static_cast<size_t>(std::countr_zero(free_bits));
                size_t slot = w * 64 + bit;
                if (slot >= m_num_slots)
                {
                    free_bits = 0;
                    break;
                }
                claimed |= (uint64_t(1) << bit);
                free_bits &= free_bits - 1;
                ++tmp;
                ++local_found;
            }

            if (claimed == 0)
                break;
            new_word = word | claimed;
        }
        while (!m_words[w].compare_exchange_weak(word, new_word, std::memory_order_acquire, std::memory_order_relaxed));

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

size_t bitmap::alloc_bit(size_t limit) noexcept
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

                uint64_t new_word = word | (uint64_t(1) << bit);
                if (m_words[w].compare_exchange_weak(word, new_word, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    m_free_count.fetch_sub(1, std::memory_order_relaxed);
                    if ((new_word | tail_mask) == ~uint64_t(0))
                        m_hint.store(w + 1, std::memory_order_relaxed);
                    return slot;
                }
                // word reloaded by CAS failure — loop recomputes effective
            }
        }
    }
    return static_cast<size_t>(-1);
}

size_t bitmap::alloc_bits_batch(size_t count, size_t limit, size_t out[]) noexcept
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
    for (size_t w = 0; w < limit_words && found < count; ++w)
    {
        uint64_t word = m_words[w].load(std::memory_order_relaxed);

        uint64_t claimed = 0;
        size_t local_found = 0;
        uint64_t new_word = 0;

        do
        {
            claimed = 0;
            local_found = 0;
            uint64_t effective = (w == limit_words - 1) ? (word | tail_mask) : word;
            if (effective == ~uint64_t(0))
                break;

            uint64_t free_bits = ~effective;
            size_t tmp = found;

            while (free_bits && tmp < count)
            {
                size_t bit = static_cast<size_t>(std::countr_zero(free_bits));
                size_t slot = w * 64 + bit;
                if (slot >= limit_slots)
                {
                    free_bits = 0;
                    break;
                }
                claimed |= (uint64_t(1) << bit);
                free_bits &= free_bits - 1;
                ++tmp;
                ++local_found;
            }

            if (claimed == 0)
                break;
            new_word = word | claimed;
        }
        while (!m_words[w].compare_exchange_weak(word, new_word, std::memory_order_acquire, std::memory_order_relaxed));
        // word reloaded on CAS failure — effective recomputed at top of do-while

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
    return found;
}

void bitmap::free_bit(size_t slot) noexcept
{
    assert(slot < m_num_slots);
    size_t word_idx = slot >> 6;
    uint64_t mask = uint64_t(1) << (slot & 63);
    uint64_t prev = m_words[word_idx].fetch_and(~mask, std::memory_order_release);
    assert((prev & mask) != 0 && "double free");
    (void)prev;
    m_free_count.fetch_add(1, std::memory_order_relaxed);
}

void bitmap::free_bits_batch(const size_t slots[], size_t count) noexcept
{
    for (size_t i = 0; i < count; ++i)
    {
        size_t word_idx = slots[i] >> 6;
        uint64_t mask = uint64_t(1) << (slots[i] & 63);
        m_words[word_idx].fetch_and(~mask, std::memory_order_release);
    }
    m_free_count.fetch_add(count, std::memory_order_relaxed);
}

void bitmap::reset() noexcept
{
    std::memset(m_words, 0, m_num_words * sizeof(uint64_t));
    size_t tail = m_num_slots % 64;
    if (tail != 0)
        m_words[m_num_words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);
    m_free_count.store(m_num_slots, std::memory_order_relaxed);
    m_hint.store(0, std::memory_order_relaxed);
}

void bitmap::reset_to(size_t active_words, size_t active_slots) noexcept
{
    if (active_words == 0 || active_slots == 0) return;
    std::memset(m_words, 0, active_words * sizeof(uint64_t));
    size_t tail = active_slots % 64;
    if (tail != 0)
        m_words[active_words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);
    m_free_count.store(active_slots, std::memory_order_relaxed);
    m_hint.store(0, std::memory_order_relaxed);
}

bool bitmap::is_range_empty(size_t word_start, size_t word_count) const noexcept
{
    for (size_t w = word_start; w < word_start + word_count; ++w)
        if (m_words[w].load(std::memory_order_relaxed) != 0)
            return false;
    return true;
}

bool bitmap::is_slot_set(size_t slot) const noexcept
{
    return (m_words[slot >> 6].load(std::memory_order_relaxed) >> (slot & 63)) & 1;
}

void bitmap::set_slot(size_t slot) noexcept
{
    m_words[slot >> 6].fetch_or(uint64_t(1) << (slot & 63), std::memory_order_relaxed);
}

void bitmap::clear_slot(size_t slot) noexcept
{
    m_words[slot >> 6].fetch_and(~(uint64_t(1) << (slot & 63)), std::memory_order_relaxed);
}

size_t bitmap::free_count() const noexcept
{
    return m_free_count.load(std::memory_order_relaxed);
}
void bitmap::set_free_count(size_t n) noexcept
{
    m_free_count.store(n, std::memory_order_relaxed);
}
void bitmap::fetch_add_free_count(size_t n) noexcept
{
    m_free_count.fetch_add(n, std::memory_order_relaxed);
}
size_t bitmap::num_slots() const noexcept
{
    return m_num_slots;
}
size_t bitmap::num_words() const noexcept
{
    return m_num_words;
}
bool bitmap::is_init() const noexcept
{
    return m_words != nullptr;
}

} // namespace AL
