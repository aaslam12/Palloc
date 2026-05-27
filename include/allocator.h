#pragma once

#include "arena.h"
#include "pool.h"
#include "slab.h"
#include <cstddef>

namespace AL
{

template<typename T>
struct arena_allocator
{
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    arena<>* m_arena;

    arena_allocator(arena<>* a) noexcept : m_arena(a)
    {}

    template<typename U>
    arena_allocator(const arena_allocator<U>& other) noexcept : m_arena(other.m_arena)
    {}

    [[nodiscard]] pointer allocate(size_type n)
    {
        if (n == 0)
            return nullptr;
        void* ptr = m_arena->alloc(n * sizeof(T));
        if (!ptr)
            throw std::bad_alloc();
        return static_cast<pointer>(ptr);
    }

    void deallocate(pointer p, size_type n) noexcept
    {
        (void)p;
        (void)n;
        // no-op: memory is freed when the arena is destroyed
    }

    template<typename U>
    bool operator==(const arena_allocator<U>& other) const noexcept
    {
        return m_arena == other.m_arena;
    }

    template<typename U>
    bool operator!=(const arena_allocator<U>& other) const noexcept
    {
        return m_arena != other.m_arena;
    }
};

template<typename T>
struct slab_allocator
{
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    default_slab* m_slab;

    slab_allocator(default_slab* s) noexcept : m_slab(s)
    {}

    template<typename U>
    slab_allocator(const slab_allocator<U>& other) noexcept : m_slab(other.m_slab)
    {}

    [[nodiscard]] pointer allocate(size_type n)
    {
        if (n == 0)
            return nullptr;
        void* ptr = m_slab->palloc(n * sizeof(T));
        if (!ptr)
            throw std::bad_alloc();
        return static_cast<pointer>(ptr);
    }

    void deallocate(pointer p, size_type n) noexcept
    {
        if (p && n > 0)
            m_slab->free(p, n * sizeof(T));
    }

    template<typename U>
    bool operator==(const slab_allocator<U>& other) const noexcept
    {
        return m_slab == other.m_slab;
    }

    template<typename U>
    bool operator!=(const slab_allocator<U>& other) const noexcept
    {
        return m_slab != other.m_slab;
    }
};

template<typename T>
struct pool_allocator
{
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    // pool block size must equal sizeof(T)
    pool* m_pool;

    pool_allocator(pool* p) noexcept : m_pool(p)
    {}

    template<typename U>
    pool_allocator(const pool_allocator<U>& other) noexcept : m_pool(other.m_pool)
    {}

    [[nodiscard]] pointer allocate(size_type n)
    {
        if (n != 1)
        {
            // pool only supports single-object allocations
            throw std::bad_alloc();
        }
        void* ptr = m_pool->alloc();
        if (!ptr)
            throw std::bad_alloc();
        return static_cast<pointer>(ptr);
    }

    void deallocate(pointer p, size_type n) noexcept
    {
        if (p && n == 1)
            m_pool->free(p);
    }

    template<typename U>
    bool operator==(const pool_allocator<U>& other) const noexcept
    {
        return m_pool == other.m_pool;
    }

    template<typename U>
    bool operator!=(const pool_allocator<U>& other) const noexcept
    {
        return m_pool != other.m_pool;
    }
};
} // namespace AL
