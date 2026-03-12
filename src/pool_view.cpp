#include "pool_view.h"

namespace AL
{

void pool_view::init_from_region(void* base, size_t block_size, size_t block_count) noexcept
{}

void* pool_view::alloc() noexcept
{
    return nullptr;
}

void* pool_view::calloc() noexcept
{
    return nullptr;
}

void pool_view::free(void* ptr) noexcept
{}

void pool_view::free_batch(std::span<void*> ptrs, size_t count) noexcept
{}

void pool_view::reset() noexcept
{}

size_t pool_view::free_count() const noexcept
{
    return 0;
}

size_t pool_view::block_count() const noexcept
{
    return 0;
}

size_t pool_view::block_size() const noexcept
{
    return 0;
}

size_t pool_view::capacity() const noexcept
{
    return 0;
}

bool pool_view::owns(const void* ptr) const noexcept
{
    return false;
}

bool pool_view::is_initialized() const noexcept
{
    return false;
}

std::byte* pool_view::memory_start() const noexcept
{
    return nullptr;
}

std::byte* pool_view::memory_end() const noexcept
{
    return nullptr;
}
} // namespace AL
