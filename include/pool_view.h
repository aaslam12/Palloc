#pragma once

#include "palloc_atomic.h"
#include <cstddef>
#include <span>

namespace AL
{

// pool_view: flat bitmap allocator over a contiguous memory region.
//
// The bitmap covers all blocks up to virtual_block_ceiling, committed upfront.
// The payload region is virtual_alloc'd (reserved) and committed in chunks on
// demand by the owning slab. alloc/free only scan up to committed_blocks words,
// so uncommitted payload is never touched.
class pool_view
{
public:
    pool_view() noexcept = default;

    pool_view(const pool_view&) = delete;
    pool_view& operator=(const pool_view&) = delete;

    pool_view(pool_view&& other) noexcept;
    pool_view& operator=(pool_view&& other) noexcept;

    // standalone path: bitmap embedded at start of region, fixed capacity
    void init_from_region(void* base, size_t block_size, size_t block_count) noexcept;

    // growable path: flat bitmap pre-allocated externally, payload lazy-committed
    // bitmap must cover virtual_block_ceiling blocks and be zeroed by caller
    void init_from_region(void* payload,
                          palloc_atomic<uint64_t>* bitmap,
                          size_t block_size,
                          size_t committed_blocks,
                          size_t virtual_block_ceiling,
                          size_t blocks_per_chunk) noexcept;

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

    // growth interface
    [[nodiscard]] size_t committed_blocks() const noexcept;
    [[nodiscard]] size_t virtual_block_ceiling() const noexcept;
    [[nodiscard]] size_t blocks_per_chunk() const noexcept;

    bool try_reserve_chunk(size_t& old_committed) noexcept;
    void advance_committed(size_t new_committed) noexcept;

    [[nodiscard]] bool is_chunk_empty(size_t chunk_idx) const noexcept;
    void decommit_blocks(size_t new_committed) noexcept;

    [[nodiscard]] static size_t required_region_size(size_t block_size, size_t block_count) noexcept;
    [[nodiscard]] static size_t bitmap_bytes_for(size_t block_count) noexcept;

private:
    std::byte* m_memory = nullptr;
    size_t m_block_size = 0;
    size_t m_block_shift = 0;
    size_t m_blocks_per_chunk = 0;
    palloc_atomic<size_t> m_committed_blocks{0};
    palloc_atomic<size_t> m_reserved_blocks{0};
    size_t m_virtual_block_ceiling = 0;
    palloc_atomic<size_t> m_free_count{0};
    palloc_atomic<uint64_t>* m_bitmap = nullptr; // covers all virtual_block_ceiling blocks
    size_t m_bitmap_words = 0;                   // (virtual_block_ceiling + 63) / 64
};

} // namespace AL
