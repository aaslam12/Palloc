#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace AL
{

class pool_view
{
public:
    pool_view() noexcept = default;

    // region must hold at least required_region_size(block_size, block_count) bytes.
    // base must be cache line aligned
    void init_from_region(void* base, size_t block_size, size_t block_count) noexcept;

    [[nodiscard]] void* alloc() noexcept;
    [[nodiscard]] void* calloc() noexcept;

    // batch-allocate up to `count` blocks into out[].
    // returns the number actually allocated (may be < count if pool has fewer free blocks).
    // significantly faster than calling alloc() in a loop: single bitmap scan pass.
    [[nodiscard]] size_t alloc_batch(size_t count, void* out[]) noexcept;

    void free(void* ptr) noexcept;
    void free_batch(std::span<void*> ptrs) noexcept;
    void reset() noexcept;

    [[nodiscard]] size_t free_count() const noexcept;
    [[nodiscard]] size_t block_count() const noexcept;
    [[nodiscard]] size_t block_size() const noexcept;
    [[nodiscard]] size_t capacity() const noexcept;
    [[nodiscard]] bool owns(const void* ptr) const noexcept;
    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] std::byte* memory_start() const noexcept;
    [[nodiscard]] std::byte* memory_end() const noexcept;

    // computes the minimum region size needed for a given block_size and block_count.
    // includes bitmap + alignment padding + payload.
    [[nodiscard]] static size_t required_region_size(size_t block_size, size_t block_count) noexcept;

private:
    std::byte* m_memory = nullptr; // first payload block (after bitmap + padding)
    uint64_t* m_bitmap = nullptr;  // bitmap at start of region
    size_t m_block_size = 0;
    size_t m_block_count = 0;
    size_t m_free_count = 0;
    size_t m_bitmap_words = 0;
    size_t m_hint = 0;       // first bitmap word that may have a free bit
    size_t m_block_shift = 0; // log2(m_block_size) — used for shift-based indexing
};

} // namespace AL
