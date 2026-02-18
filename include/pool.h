#pragma once

#include <cstddef>

namespace AL
{
class pool
{
    struct free_node
    {
        free_node* next;
    };

public:
    pool();
    pool(size_t block_size, size_t block_count);
    ~pool();

    pool(const pool&) = delete;
    pool& operator=(const pool&) = delete;

    void init(size_t block_size, size_t block_count);

    // allocates a block of memory from the pool
    // returns: nullptr if failed, else the memory address of the block of memory
    void* alloc();

    // allocates a block of memory from the pool
    // also zeroes out the memory returned
    // returns: nullptr if failed, else the memory address of the block of memory
    void* calloc();

    // frees the entire pool but keeps it alive to reuse
    // returns: -1 if failed
    void reset();

    // frees the block
    // returns: -1 if failed
    void free(void* ptr);

    // returns get number of free bytes
    size_t get_free_space() const;

    // gets the total amount of bytes that can be used by the pool
    size_t get_capacity() const;

    size_t get_block_size() const;
    size_t get_block_count() const;

private:
    std::byte* memory;
    size_t capacity;
    size_t free_count;

    size_t block_size;
    size_t block_count;
    free_node* free_list;

    bool owns(void* ptr) const;
    void init_free_list();

    void check_asserts() const;
};
} // namespace AL
