#pragma once

#include "bitmap.h"
#include "palloc_atomic.h"
#include <cstddef>
#include <functional>
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

    // Embedded bitmap path (pool owns its region). Used by standalone pool/pool_view.
    void init_from_region(void* base, size_t block_size, size_t block_count) noexcept;

    // External bitmap path (slab owns the bitmap). Used by slab.
    // committed_blocks: initially committed block count (scan limit).
    // virtual_block_ceiling: total virtual block capacity (bitmap size).
    // blocks_per_chunk: growth increment = initial num_blocks.
    void init_from_region(void* base, void* bitmap_mem, size_t block_size,
                          size_t committed_blocks, size_t virtual_block_ceiling,
                          size_t blocks_per_chunk) noexcept;

    [[nodiscard]] void* alloc() noexcept;
    [[nodiscard]] void* calloc() noexcept;
    [[nodiscard]] size_t alloc_batch(size_t count, void* out[]) noexcept;

    void free(void* ptr) noexcept;
    void free_batch(std::span<void*> ptrs) noexcept;
    void reset() noexcept;

    [[nodiscard]] size_t free_count() const noexcept;
    [[nodiscard]] size_t block_count() const noexcept;   // committed blocks
    [[nodiscard]] size_t block_size() const noexcept;
    [[nodiscard]] size_t capacity() const noexcept;      // committed capacity in bytes
    [[nodiscard]] bool   owns(const void* ptr) const noexcept;
    [[nodiscard]] bool   is_initialized() const noexcept;
    [[nodiscard]] std::byte* memory_start() const noexcept;
    [[nodiscard]] std::byte* memory_end() const noexcept;

    // Growth interface — used by slab
    [[nodiscard]] size_t committed_blocks() const noexcept;
    [[nodiscard]] size_t virtual_block_ceiling() const noexcept;
    [[nodiscard]] size_t blocks_per_chunk() const noexcept;
    // Step 1: atomically reserve the next chunk. Returns true if this thread won.
    // old_committed is set to the pre-reservation committed count (= chunk start offset).
    bool try_reserve_chunk(size_t& old_committed) noexcept;
    // Step 2: called after virtual_commit succeeds. Makes new blocks visible to alloc.
    void advance_committed(size_t new_committed) noexcept;

    // Decommit interface — used by slab after detecting an empty chunk
    // Returns chunk index if the chunk containing ptr's block just became empty, else -1.
    [[nodiscard]] size_t chunk_index_if_empty(void* ptr) const noexcept;

    // Check if a range of bitmap words is all-zero (chunk empty check for shrink).
    [[nodiscard]] bool is_chunk_empty(size_t word_start, size_t word_count) const noexcept;

    // Roll back committed_blocks by chunk_size after decommit. NOT thread-safe.
    void decommit_blocks(size_t chunk_start_block, size_t chunk_block_count) noexcept;

    [[nodiscard]] static size_t required_region_size(size_t block_size, size_t block_count) noexcept;

private:
    std::byte*            m_memory               = nullptr;
    size_t                m_block_size            = 0;
    size_t                m_block_shift           = 0;
    size_t                m_block_count           = 0; // committed block count (== m_committed_blocks for capacity/owns)
    palloc_atomic<size_t> m_committed_blocks{0};
    palloc_atomic<size_t> m_reserved_blocks{0};  // chunks reserved (CAS'd) but may not be committed yet
    size_t                m_virtual_block_ceiling = 0;
    size_t                m_blocks_per_chunk      = 0;
    bitmap                m_bitmap;
};

} // namespace AL
