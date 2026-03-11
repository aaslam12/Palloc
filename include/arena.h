#pragma once

#include "platform.h"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <new>

#ifndef PALLOC_DEFAULT_ALIGNMENT
#ifdef PALLOC_8BYTE_ALIGNMENT
#define PALLOC_DEFAULT_ALIGNMENT 8
#else
#define PALLOC_DEFAULT_ALIGNMENT alignof(std::max_align_t) // usually 16
#endif
#endif

namespace AL
{
template<size_t Talignment = PALLOC_DEFAULT_ALIGNMENT>
class arena
{
public:
    explicit arena(size_t bytes) : memory(nullptr), used(0), capacity(0)
    {
        size_t page_size = AL::platform_mem::page_size();
        capacity = ((bytes + page_size - 1) / page_size) * page_size;
        void* ptr = AL::platform_mem::alloc(capacity);
        if (ptr == nullptr)
        {
            throw std::bad_alloc();
        }
        memory = static_cast<std::byte*>(ptr);
    }

    ~arena()
    {
        if (memory != nullptr)
        {
            AL::platform_mem::free(memory, capacity);
        }
    }

    arena(const arena&) = delete;
    arena& operator=(const arena&) = delete;

    arena(arena&& other) noexcept : memory(other.memory), used(other.used.load()), capacity(other.capacity)
    {
        other.memory = nullptr;
        other.used = 0;
        other.capacity = 0;
    }

    arena& operator=(arena&& other) noexcept
    {
        if (this != &other)
        {
            if (memory != nullptr)
            {
                AL::platform_mem::free(memory, capacity);
            }
            memory = other.memory;
            used = other.used.load();
            capacity = other.capacity;
            other.memory = nullptr;
            other.used = 0;
            other.capacity = 0;
        }
        return *this;
    }

    [[nodiscard]] void* alloc(size_t length)
    {
        if (length == 0 || memory == nullptr)
            return nullptr;

        // zero runtime overhead for calculation.
        size_t total_to_add = (length + Talignment - 1) & ~(Talignment - 1);
        size_t offset = used.fetch_add(total_to_add, std::memory_order_acq_rel);

        if (offset + total_to_add > capacity)
        {
            return nullptr;
        }

        return memory + offset;
    }

    [[nodiscard]] void* calloc(size_t length)
    {
        void* ptr = alloc(length);
        if (ptr != nullptr)
        {
            std::memset(ptr, 0, length);
        }
        return ptr;
    }

    int reset()
    {
        used = 0;
        return 0;
    }

    int clear()
    {
        if (memory != nullptr)
        {
            bool ok = AL::platform_mem::free(memory, capacity);
            memory = nullptr;
            if (!ok)
                return -1;
        }
        used.store(0);
        capacity = 0;
        return 0;
    }

    size_t get_used() const
    {
        return used.load(std::memory_order::relaxed);
    }

    size_t get_capacity() const
    {
        return capacity;
    }

private:
    std::byte* memory;
    std::atomic<size_t> used;
    size_t capacity;
};
} // namespace AL
