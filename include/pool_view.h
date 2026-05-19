#pragma once

#include "bitmap.h"
#include "palloc_atomic.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace AL
{

class pool_view
{
public:
    pool_view() noexcept = default;

    pool_view(const pool_view&) = delete;
    pool_view& operator=(const pool_view&) = delete;

    pool_view(pool_view&& other) noexcept;
    pool_view& operator=(pool_view&& other) noexcept;

    // region must hold at least required_region_size(block_size, block_count) bytes.
    // base must be cache line aligned. Bitmap is embedded at the start of the region.
    void init_from_region(void* base, size_t block_size, size_t block_count) noexcept;

    // Payload region starts at base. Bitmap is provided externally.
    void init_from_region(void* base, void* bitmap_mem, size_t block_size, size_t block_count) noexcept;

    [[nodiscard]] void* alloc() noexcept;
    [[nodiscard]] void* calloc() noexcept;

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

    [[nodiscard]] static size_t required_region_size(size_t block_size, size_t block_count) noexcept;

private:
    std::byte* m_memory     = nullptr;
    size_t     m_block_size = 0;
    size_t     m_block_count = 0;
    size_t     m_block_shift = 0; // log2(m_block_size)
    bitmap     m_bitmap;
};

} // namespace AL
