#pragma once

#include "palloc_atomic.h"
#include <cstddef>
#include <span>

namespace AL
{

// pool_view with two-level bitmap.
// standalone path: m_embedded_bitmap holds a flat bitmap inside the region.
class pool_view
{
public:
    pool_view() noexcept = default;

    pool_view(const pool_view&) = delete;
    pool_view& operator=(const pool_view&) = delete;

    pool_view(pool_view&& other) noexcept;
    pool_view& operator=(pool_view&& other) noexcept;

    // embedded bitmap path (standalone pool/pool_view, no growth)
    void init_from_region(void* base, size_t block_size, size_t block_count) noexcept;

    // two-level bitmap path
    // chunk_bitmaps[0] must already point to the committed fine bitmap for the initial chunk.
    // committed_blocks = initial num_blocks.
    // virtual_block_ceiling = full virtual capacity.
    // blocks_per_chunk = growth increment (= initial num_blocks).
    void init_from_region(void* base,
                          palloc_atomic<uint64_t>** chunk_bitmaps,
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

    // growth interface - used by slab internally
    [[nodiscard]] size_t committed_blocks() const noexcept;
    [[nodiscard]] size_t virtual_block_ceiling() const noexcept;
    [[nodiscard]] size_t blocks_per_chunk() const noexcept;

    // returns true if this thread won the reservation CAS
    // old_committed is set to the pre-reservation value (= start block of new chunk)
    bool try_reserve_chunk(size_t& old_committed) noexcept;

    // called by slab after virtual_commit + fine bitmap allocation - makes new blocks visible
    void advance_committed(size_t new_committed) noexcept;

    // returns true if all blocks in chunk_idx are free
    [[nodiscard]] bool is_chunk_empty(size_t chunk_idx) const noexcept;

    // rolls back committed/reserved counts to new_committed - NOT thread-safe
    void decommit_blocks(size_t new_committed) noexcept;

    [[nodiscard]] static size_t required_region_size(size_t block_size, size_t block_count) noexcept;
    [[nodiscard]] static size_t fine_bitmap_bytes(size_t blocks_per_chunk) noexcept;

private:
    // alloc one slot from chunk_idx's fine bitmap
    // returns pool-absolute slot index, or -1 if full
    [[nodiscard]] size_t alloc_from_chunk(size_t chunk_idx) noexcept;
    size_t alloc_batch_from_chunk(size_t chunk_idx, size_t count, size_t out[]) noexcept;
    void free_to_chunk(size_t chunk_idx, size_t bit) noexcept;

    std::byte* m_memory = nullptr;
    size_t m_block_size = 0;
    size_t m_block_shift = 0;
    size_t m_blocks_per_chunk = 0;
    size_t m_words_per_chunk = 0; // (blocks_per_chunk + 63) / 64
    palloc_atomic<size_t> m_committed_blocks{0};
    palloc_atomic<size_t> m_reserved_blocks{0};
    size_t m_virtual_block_ceiling = 0;
    palloc_atomic<size_t> m_free_count{0};

    palloc_atomic<uint64_t>** m_chunk_bitmaps = nullptr;  // non-owning, allocated by slab
    palloc_atomic<uint64_t>* m_embedded_bitmap = nullptr; // standalone path only
    size_t m_embedded_num_words = 0;
    palloc_atomic<size_t> m_hint{0};
};

} // namespace AL
