#pragma once

#include "palloc_atomic.h"
#include <cstddef>
#include <cstdint>

namespace AL
{

// Flat atomic bitmap over an externally provided memory region.
// Non-owning: does not allocate or free memory.
// One bit per slot: 0 = free, 1 = allocated.
class bitmap
{
public:
    bitmap() noexcept = default;

    bitmap(const bitmap&) = delete;
    bitmap& operator=(const bitmap&) = delete;

    bitmap(bitmap&& other) noexcept;
    bitmap& operator=(bitmap&& other) noexcept;

    // base: pointer to memory large enough for required_size(num_slots) bytes, pre-zeroed.
    // Trailing bits beyond num_slots in the last word are pre-set to 1 so scans terminate correctly.
    void init(void* base, size_t num_slots) noexcept;

    // Atomically claim one free slot. Returns slot index, or (size_t)-1 if full.
    [[nodiscard]] size_t alloc_bit() noexcept;

    // Atomically claim up to count free slots. Writes slot indices into out[]. Returns count claimed.
    size_t alloc_bits_batch(size_t count, size_t out[]) noexcept;

    // Atomically free a slot.
    void free_bit(size_t slot) noexcept;

    // Batch free using word-level accumulation: one fetch_and per touched word.
    void free_bits_batch(const size_t slots[], size_t count) noexcept;

    // Reset all slots to free. Not thread-safe.
    void reset() noexcept;

    // Returns true if every bit in words [word_start, word_start + word_count) is 0.
    [[nodiscard]] bool is_range_empty(size_t word_start, size_t word_count) const noexcept;

    [[nodiscard]] bool is_slot_set(size_t slot) const noexcept;
    void set_slot(size_t slot) noexcept;
    void clear_slot(size_t slot) noexcept;

    [[nodiscard]] size_t free_count() const noexcept;
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
