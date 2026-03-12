#pragma once

#include <cstddef>
#include <span>
namespace AL
{

class pool_view
{
public:
    void init_from_region(void* base, size_t block_size, size_t block_count) noexcept;

    [[nodiscard]] void* alloc() noexcept;
    [[nodiscard]] void* calloc() noexcept;

    void free(void* ptr) noexcept;
    void free_batch(std::span<void*> ptrs, size_t count) noexcept;
    void reset() noexcept;

    size_t free_count() const noexcept;
    size_t block_count() const noexcept;
    size_t block_size() const noexcept;
    size_t capacity() const noexcept;
    bool owns(const void* ptr) const noexcept;
    bool is_initialized() const noexcept;
    std::byte* memory_start() const noexcept;
    std::byte* memory_end() const noexcept;
};

} // namespace AL
