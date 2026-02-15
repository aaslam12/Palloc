#include "arena.h"
#include <cassert>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

namespace AL
{
arena::arena(size_t bytes) : memory(nullptr), used(0), capacity(0)
{
    int page_size = getpagesize();

    // round up to next page boundary
    capacity = ((bytes + page_size - 1) / page_size) * page_size;

    void* ptr = mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (ptr == MAP_FAILED)
    {
        throw std::bad_alloc();
    }

    memory = static_cast<std::byte*>(ptr);
    used = 0;
}

arena::~arena()
{
    if (memory == nullptr)
        return;

    int result = munmap(memory, capacity);

#if SLABALLOCATOR_DEBUG
    // something went wrong.
    if (result == -1)
    {
        std::cerr << "WARNING: munmap failed in arena destructor\n";
    }
#endif // SLABALLOCATOR_DEBUG
}

void* arena::alloc(size_t length)
{
    if (length == 0 || memory == nullptr)
        return nullptr;

    // if we do not have enough space left in the page
    if (length > (capacity - used))
        return nullptr;

    std::byte* result = memory + used;
    used += length;

    return result;
}

void* arena::calloc(size_t length)
{
    void* ptr = alloc(length);

    if (ptr != nullptr)
    {
        std::memset(ptr, 0, length);
    }

    return ptr;
}

int arena::reset()
{
    used = 0;
    return 0;
}

size_t arena::get_used() const
{
    return used;
}

size_t arena::get_capacity() const
{
    return capacity;
}
} // namespace AL
