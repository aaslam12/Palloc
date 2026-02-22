#include "arena.h"
#include <atomic>
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

    void* ptr = mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

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

#if PALLOC_DEBUG
    // something went wrong.
    if (result == -1)
    {
        std::cerr << "WARNING: munmap failed in arena destructor\n";
    }
#endif // PALLOC_DEBUG
}

arena::arena(arena&& other) noexcept : memory(other.memory), used(other.used.load()), capacity(other.capacity)
{
    other.reset();
    other.capacity = 0;
    other.used = 0;
    other.memory = nullptr;
}

arena& arena::operator=(arena&& other) noexcept
{
    if (this == &other)
        return *this;

    if (memory != nullptr)
    {
        munmap(memory, capacity);
    }

    memory = other.memory;
    used = other.used.load();
    capacity = other.capacity;

    other.reset();
    other.capacity = 0;
    other.used = 0;
    other.memory = nullptr;
    return *this;
}

void* arena::alloc(size_t length)
{
    if (length == 0 || memory == nullptr)
        return nullptr;

    size_t current;
    while (true)
    {
        current = used.load(std::memory_order::relaxed);

        // if we do not have enough space left in the page
        if (length > (capacity - current))
            return nullptr;

        if (used.compare_exchange_weak(current, current + length, std::memory_order_release, std::memory_order_relaxed))
            return memory + current;
    }
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

int arena::clear()
{
    if (memory != nullptr)
    {
        int result = munmap(memory, capacity);
        memory = nullptr;

        if (result != 0)
            return result;
    }

    used.store(0);
    capacity = 0;
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
