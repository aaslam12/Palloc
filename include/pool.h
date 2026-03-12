#pragma once

#include "pool_view.h"
#include <atomic>
#include <cstddef>
#include <mutex>
#include <new>

namespace AL
{
template<typename Tconfig>
class slab;

class alignas(std::hardware_destructive_interference_size) pool
{
public:
    template<typename Tconfig>
    friend class slab;

    pool();
    pool(size_t block_size, size_t block_count);
    ~pool();

    pool(const pool&) = delete;
    pool& operator=(const pool&) = delete;
    pool(pool&&) noexcept;
    pool& operator=(pool&&) noexcept;

    void init(size_t block_size, size_t block_count);

    // non-owning initialization: the pool does not mmap or own the memory.
    // the caller (typically slab) is responsible for the lifetime of the region.
    // base must be aligned to at least block_size.
    void init_from_region(void* base, size_t block_size, size_t block_count);

    // allocates a block of memory from the pool
    // returns properly aligned memory
    // thread-safe
    // returns: nullptr if failed, else the memory address of the block of memory
    [[nodiscard]] void* alloc();

    // allocates a block of memory from the pool
    // also zeroes out the memory returned
    // returns properly aligned memory
    // thread-safe
    // returns: nullptr if failed, else the memory address of the block of memory
    [[nodiscard]] void* calloc();

    // frees the entire pool but keeps it alive to reuse
    // thread-safe
    void reset();

    // frees the block
    // thread-safe
    void free(void* ptr);

    // already thread safe (atomic)
    // returns number of free bytes
    size_t get_free_space() const;

    // gets the total amount of bytes that can be used by the pool
    size_t get_capacity() const;

    size_t get_block_size() const;
    size_t get_block_count() const;
    void clear();

    std::byte* get_memory_start() const
    {
        return m_view.memory_start();
    }
    std::byte* get_memory_end() const
    {
        return m_view.memory_end();
    }

private:
    std::byte* m_region = nullptr; // owned mmap'd memory
    size_t m_region_size = 0;      // total mmap'd size (for munmap)
    pool_view m_view;              // bitmap-based allocator (non-owning)
    std::atomic<size_t> m_free_count{0};
    mutable std::mutex m_mutex;

    bool owns(void* ptr) const;
    void check_asserts() const;

    size_t alloc_batched_internal(size_t num_objects, void* out[]);
    void free_batched_internal(size_t num_objects, void* in[]);
};
} // namespace AL
