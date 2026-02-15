#pragma once

#include <cstddef>

class arena
{
public:
    arena();

    ~arena();

    // allocates a block of memory of specified length from the arena
    // returns: nullptr if failed, else the memory address of the block of memory
    void* alloc(size_t length);

    // frees the entire arena but keeps it alive to reuse
    // returns: -1 if failed
    int reset();

    // gets the amount of bytes used by the arena
    size_t get_used() const;

    // gets the total amount of bytes that can be used by the arena
    size_t get_capacity() const;

private:
    void* memory;
    size_t used;
    size_t capacity;
};
