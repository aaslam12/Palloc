#pragma once

#include "palloc_atomic.h"
#include <cstddef>
#include <cstdint>

namespace AL
{

// flat atomic bitmap over an external memory region
// non-owning: does not allocate or free memory
// 0 = free, 1 = allocated
class bitmap
{
public:
    bitmap() noexcept = default;

    bitmap(const bitmap&) = delete;
    bitmap& operator=(const bitmap&) = delete;

    bitmap(bitmap&& other) noexcept;
    bitmap& operator=(bitmap&& other) noexcept;

    // base must be pre-zeroed and large enough for required_size(num_slots) bytes
    // trailing bits beyond num_slots are pre-set to 1 so scans terminate correctly
    void init(void* base, size_t num_slots) noexcept;

    // atomically claim one free slot - returns slot index or (size_t)-1 if full
    [[nodiscard]] size_t alloc_bit() noexcept;

    // atomically claim up to count free slots - writes indices into out[], returns count claimed
    size_t alloc_bits_batch(size_t count, size_t out[]) noexcept;

    // like alloc_bit but scans only up to limit slots
    [[nodiscard]] size_t alloc_bit(size_t limit) noexcept;

    // like alloc_bits_batch but scans only up to limit slots
    size_t alloc_bits_batch(size_t count, size_t limit, size_t out[]) noexcept;

    // atomically free a slot
    void free_bit(size_t slot) noexcept;

    // batch free - accumulates masks per word, one fetch_and per touched word
    void free_bits_batch(const size_t slots[], size_t count) noexcept;

    // reset all slots to free - not thread-safe
    void reset() noexcept;

    // reset only the first active_words words - not thread-safe
    void reset_to(size_t active_words, size_t active_slots) noexcept;

    // returns true if every bit in [word_start, word_start + word_count) is 0
    [[nodiscard]] bool is_range_empty(size_t word_start, size_t word_count) const noexcept;

    [[nodiscard]] bool is_slot_set(size_t slot) const noexcept;
    void set_slot(size_t slot) noexcept;
    void clear_slot(size_t slot) noexcept;

    [[nodiscard]] size_t free_count() const noexcept;
    void set_free_count(size_t n) noexcept;
    void fetch_add_free_count(size_t n) noexcept;
    [[nodiscard]] size_t num_slots() const noexcept;
    [[nodiscard]] size_t num_words() const noexcept;
    [[nodiscard]] bool is_init() const noexcept;

    [[nodiscard]] static constexpr size_t required_size(size_t num_slots) noexcept
    {
        return ((num_slots + 63) / 64) * sizeof(uint64_t);
    }

private:
    palloc_atomic<uint64_t>* m_words = nullptr;
    size_t m_num_words = 0;
    size_t m_num_slots = 0;
    palloc_atomic<size_t> m_free_count{0};
    palloc_atomic<size_t> m_hint{0};
};

} // namespace AL
