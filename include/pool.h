#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>

namespace AL
{
class slab;

class pool
{
    struct free_node
    {
        free_node* next;
    };

public:
    friend class slab;

    pool();
    pool(size_t block_size, size_t block_count);
    ~pool();

    pool(const pool&) = delete;
    pool& operator=(const pool&) = delete;
    pool(pool&&) noexcept;
    pool& operator=(pool&&) noexcept;

    void init(size_t block_size, size_t block_count);

    // allocates a block of memory from the pool
    // thread-safe
    // returns: nullptr if failed, else the memory address of the block of memory
    [[nodiscard]] void* alloc();

    // allocates a block of memory from the pool
    // also zeroes out the memory returned
    // thread-safe
    // returns: nullptr if failed, else the memory address of the block of memory
    [[nodiscard]] void* calloc();

    // frees the entire pool but keeps it alive to reuse
    // thread-safe
    // returns: -1 if failed
    void reset();

    // frees the block
    // thread-safe
    // returns: -1 if failed
    void free(void* ptr);

    // already thread safe
    // returns get number of free bytes
    size_t get_free_space() const;

    // gets the total amount of bytes that can be used by the pool
    size_t get_capacity() const;

    size_t get_block_size() const;
    size_t get_block_count() const;
    void clear();

    std::byte* get_memory_start() const { return memory; }
    std::byte* get_memory_end() const { return memory + capacity; }

private:
    std::byte* memory; // pointer to the first byte of our mapped memory
    size_t capacity;
    std::atomic<size_t> free_count;

    size_t block_size;
    size_t block_count;
    free_node* free_list;
    mutable std::mutex alloc_free_mutex;

    bool owns(void* ptr) const;
    void init_free_list();

    void check_asserts() const;

    size_t alloc_batched_internal(size_t num_objects, void* out[]);
    void free_batched_internal(size_t num_objects, void* in[]);
};
} // namespace AL
