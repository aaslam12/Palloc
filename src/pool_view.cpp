#include "pool_view.h"
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>

namespace AL
{

pool_view::pool_view(pool_view&& other) noexcept
    : m_memory(other.m_memory)
    , m_block_size(other.m_block_size)
    , m_block_shift(other.m_block_shift)
    , m_block_count(other.m_block_count)
    , m_committed_blocks(other.m_committed_blocks.load(std::memory_order_relaxed))
    , m_reserved_blocks(other.m_reserved_blocks.load(std::memory_order_relaxed))
    , m_virtual_block_ceiling(other.m_virtual_block_ceiling)
    , m_blocks_per_chunk(other.m_blocks_per_chunk)
    , m_bitmap(std::move(other.m_bitmap))
{
    other.m_memory = nullptr;
}

pool_view& pool_view::operator=(pool_view&& other) noexcept
{
    if (this == &other) return *this;
    m_memory = other.m_memory;
    m_block_size = other.m_block_size;
    m_block_shift = other.m_block_shift;
    m_block_count = other.m_block_count;
    m_committed_blocks.store(other.m_committed_blocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_reserved_blocks.store(other.m_reserved_blocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_virtual_block_ceiling = other.m_virtual_block_ceiling;
    m_blocks_per_chunk = other.m_blocks_per_chunk;
    m_bitmap = std::move(other.m_bitmap);
    other.m_memory = nullptr;
    return *this;
}

size_t pool_view::required_region_size(size_t block_size, size_t block_count) noexcept
{
    size_t bitmap_bytes = bitmap::required_size(block_count);
    size_t aligned_offset = ((bitmap_bytes + block_size - 1) / block_size) * block_size;
    return aligned_offset + block_size * block_count;
}

void pool_view::init_from_region(void* base, size_t block_size, size_t block_count) noexcept
{
    assert(base != nullptr);
    assert(block_size > 0 && std::has_single_bit(block_size));
    assert(block_size >= sizeof(void*));
    assert(block_count > 0);
    assert((reinterpret_cast<uintptr_t>(base) % block_size) == 0);

    m_block_size  = block_size;
    m_block_count = block_count;
    m_block_shift = static_cast<size_t>(std::countr_zero(block_size));
    m_committed_blocks.store(block_count, std::memory_order_relaxed);
    m_reserved_blocks.store(block_count, std::memory_order_relaxed);
    m_virtual_block_ceiling = block_count;
    m_blocks_per_chunk      = block_count;

    std::memset(base, 0, bitmap::required_size(block_count));
    m_bitmap.init(base, block_count);

    size_t bitmap_bytes = bitmap::required_size(block_count);
    void* payload_ptr = static_cast<std::byte*>(base) + bitmap_bytes;
    size_t remaining = required_region_size(block_size, block_count) - bitmap_bytes;
    void* aligned = std::align(block_size, block_size * block_count, payload_ptr, remaining);
    assert(aligned != nullptr);
    m_memory = static_cast<std::byte*>(aligned);
}

void pool_view::init_from_region(void* base, void* bitmap_mem, size_t block_size,
                                  size_t committed_blocks, size_t virtual_block_ceiling,
                                  size_t blocks_per_chunk) noexcept
{
    assert(base != nullptr);
    assert(bitmap_mem != nullptr);
    assert(block_size > 0 && std::has_single_bit(block_size));
    assert(block_size >= sizeof(void*));
    assert(committed_blocks > 0);
    assert(virtual_block_ceiling >= committed_blocks);
    assert((reinterpret_cast<uintptr_t>(base) % block_size) == 0);

    m_block_size            = block_size;
    m_block_count           = committed_blocks;
    m_block_shift           = static_cast<size_t>(std::countr_zero(block_size));
    m_committed_blocks.store(committed_blocks, std::memory_order_relaxed);
    m_reserved_blocks.store(committed_blocks, std::memory_order_relaxed);
    m_virtual_block_ceiling = virtual_block_ceiling;
    m_blocks_per_chunk      = blocks_per_chunk;

    // bitmap is pre-zeroed by caller, sized for virtual_block_ceiling
    m_bitmap.init(bitmap_mem, virtual_block_ceiling);
    m_bitmap.set_free_count(committed_blocks); // only committed slots are usable initially
    m_memory = static_cast<std::byte*>(base);
}

void* pool_view::alloc() noexcept
{
    size_t limit = m_committed_blocks.load(std::memory_order_acquire);
    size_t slot  = m_bitmap.alloc_bit(limit);
    if (slot == static_cast<size_t>(-1))
        return nullptr;
    return m_memory + (slot << m_block_shift);
}

size_t pool_view::alloc_batch(size_t count, void* out[]) noexcept
{
    size_t slots[128];
    size_t n     = count > 128 ? 128 : count;
    size_t limit = m_committed_blocks.load(std::memory_order_acquire);
    size_t found = m_bitmap.alloc_bits_batch(n, limit, slots);
    for (size_t i = 0; i < found; ++i)
        out[i] = m_memory + (slots[i] << m_block_shift);
    return found;
}

void* pool_view::calloc() noexcept
{
    void* ptr = alloc();
    if (ptr != nullptr)
        std::memset(ptr, 0, m_block_size);
    return ptr;
}

void pool_view::free(void* ptr) noexcept
{
    if (ptr == nullptr) return;
    assert(owns(ptr));
    size_t slot = static_cast<size_t>(static_cast<std::byte*>(ptr) - m_memory) >> m_block_shift;
    m_bitmap.free_bit(slot);
}

void pool_view::free_batch(std::span<void*> ptrs) noexcept
{
    size_t slots[128];
    size_t count = 0;
    for (void* ptr : ptrs)
    {
        if (ptr == nullptr) continue;
        assert(owns(ptr));
        slots[count++] = static_cast<size_t>(static_cast<std::byte*>(ptr) - m_memory) >> m_block_shift;
        if (count == 128)
        {
            m_bitmap.free_bits_batch(slots, count);
            count = 0;
        }
    }
    if (count > 0)
        m_bitmap.free_bits_batch(slots, count);
}

void pool_view::reset() noexcept
{
    // reset only the committed portion of the bitmap
    size_t committed = m_committed_blocks.load(std::memory_order_relaxed);
    size_t words = (committed + 63) / 64;
    // temporarily narrow the bitmap's view to committed words for reset
    m_bitmap.reset_to(words, committed);
}

size_t pool_view::committed_blocks() const noexcept
{
    return m_committed_blocks.load(std::memory_order_relaxed);
}

size_t pool_view::virtual_block_ceiling() const noexcept { return m_virtual_block_ceiling; }
size_t pool_view::blocks_per_chunk()      const noexcept { return m_blocks_per_chunk; }

bool pool_view::try_reserve_chunk(size_t& old_committed) noexcept
{
    old_committed = m_committed_blocks.load(std::memory_order_acquire);
    size_t next = old_committed + m_blocks_per_chunk;
    if (next > m_virtual_block_ceiling)
        return false;
    // CAS on m_reserved_blocks — only one thread commits a given chunk
    size_t expected = old_committed;
    return m_reserved_blocks.compare_exchange_strong(expected, next,
               std::memory_order_acquire, std::memory_order_relaxed);
}

void pool_view::advance_committed(size_t new_committed) noexcept
{
    size_t old = m_committed_blocks.load(std::memory_order_relaxed);
    m_bitmap.fetch_add_free_count(new_committed - old);
    m_block_count = new_committed;
    m_committed_blocks.store(new_committed, std::memory_order_release);
}

bool pool_view::is_chunk_empty(size_t word_start, size_t word_count) const noexcept
{
    return m_bitmap.is_range_empty(word_start, word_count);
}

void pool_view::decommit_blocks(size_t chunk_start_block, size_t chunk_block_count) noexcept
{
    size_t new_committed = chunk_start_block; // trim back to start of this chunk
    m_reserved_blocks.store(new_committed, std::memory_order_relaxed);
    m_committed_blocks.store(new_committed, std::memory_order_relaxed);
    m_block_count = new_committed;
    // update free_count: remove the chunk_block_count free slots from accounting
    m_bitmap.set_free_count(m_bitmap.free_count() - chunk_block_count);
}

size_t pool_view::chunk_index_if_empty(void* ptr) const noexcept
{
    size_t slot       = static_cast<size_t>(static_cast<std::byte*>(ptr) - m_memory) >> m_block_shift;
    size_t chunk_idx  = slot / m_blocks_per_chunk;
    size_t word_start = chunk_idx * m_blocks_per_chunk / 64;
    size_t words      = (m_blocks_per_chunk + 63) / 64;
    return m_bitmap.is_range_empty(word_start, words) ? chunk_idx : static_cast<size_t>(-1);
}

size_t pool_view::free_count() const noexcept  { return m_bitmap.free_count(); }
size_t pool_view::block_count() const noexcept { return m_committed_blocks.load(std::memory_order_relaxed); }
size_t pool_view::block_size()  const noexcept { return m_block_size; }
size_t pool_view::capacity()    const noexcept { return m_block_size * m_committed_blocks.load(std::memory_order_relaxed); }

bool pool_view::owns(const void* ptr) const noexcept
{
    if (ptr == nullptr || m_memory == nullptr) return false;
    auto byte_ptr = static_cast<const std::byte*>(ptr);
    size_t committed = m_committed_blocks.load(std::memory_order_relaxed);
    if (byte_ptr < m_memory || byte_ptr >= m_memory + m_block_size * committed) return false;
    return (static_cast<size_t>(byte_ptr - m_memory) & (m_block_size - 1)) == 0;
}

bool pool_view::is_initialized() const noexcept { return m_memory != nullptr; }
std::byte* pool_view::memory_start() const noexcept { return m_memory; }
std::byte* pool_view::memory_end()   const noexcept
{
    if (!m_memory) return nullptr;
    return m_memory + m_block_size * m_committed_blocks.load(std::memory_order_relaxed);
}

} // namespace AL
