#include "pool.h"
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

namespace AL
{
pool::pool()
{
    free_count = -1;
    block_size = -1;
    block_count = -1;
    capacity = -1;
    free_list = nullptr;
    memory = nullptr;
}

pool::pool(size_t block_size, size_t block_count)
    : pool()
{
    init(block_size, block_count);
}

void pool::init(size_t block_size, size_t block_count)
{
    assert(this->memory == nullptr && "pool likely already initialized correctly.");
    assert(this->capacity == (size_t)-1 && "pool likely already initialized correctly.");
    assert(this->free_count == (size_t)-1 && "pool likely already initialized correctly.");
    assert(this->block_size == (size_t)-1 && "pool likely already initialized correctly.");
    assert(this->block_count == (size_t)-1 && "pool likely already initialized correctly.");

    int page_size = getpagesize();
    if (block_size < sizeof(void*))
    {
#if SLABALLOCATOR_DEBUG
        std::cerr << "WARNING: Pool block size " << block_size << " is too small. "
                  << "Rounded up to " << sizeof(void*) << " bytes.\n";
#endif
        block_size = sizeof(void*);
    }

    this->block_size = std::bit_ceil(block_size);
    this->block_count = block_count;

    // round up to next page boundary
    size_t total_needed = this->block_size * this->block_count;
    capacity = ((total_needed + page_size - 1) / page_size) * page_size;

    void* ptr = mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (ptr == MAP_FAILED)
    {
        throw std::bad_alloc();
    }

    memory = static_cast<std::byte*>(ptr);
    init_free_list();
    free_count = block_count;
}

void pool::init_free_list()
{
    free_list = nullptr;

    // Loop backwards through block indices
    for (size_t i = block_count; i > 0; --i)
    {
        // Get pointer to block (i-1)
        std::byte* block_ptr = memory + ((i - 1) * block_size);

        // Cast to FreeNode and link
        free_node* node = reinterpret_cast<free_node*>(block_ptr);
        node->next = free_list;
        free_list = node;
    }
}

pool::~pool()
{
    if (memory == nullptr)
        return;

    // frees free list as well
    int result = munmap(memory, capacity);

#if SLABALLOCATOR_DEBUG
    if (result == -1)
    {
        std::cerr << "WARNING: munmap failed in pool destructor\n";
    }
#endif // SLABALLOCATOR_DEBUG

    memory = nullptr;
    free_list = nullptr;
}

void* pool::alloc()
{
    if (free_list == nullptr)
        return nullptr; // pool is full/uninitialized

    check_asserts();

    auto temp = free_list;
    free_list = free_list->next;
    free_count--;

    return temp;
}

void* pool::calloc()
{
    check_asserts();

    void* ptr = alloc();

    if (ptr != nullptr)
    {
        std::memset(ptr, 0, block_size);
    }

    return ptr;
}

void pool::reset()
{
    check_asserts();
    init_free_list();
    free_count = block_count;
}

bool pool::owns(void* ptr) const
{
    if (ptr == nullptr)
        return false;

    std::byte* byte_ptr = static_cast<std::byte*>(ptr);

    if (byte_ptr < memory || byte_ptr >= (memory + block_size * block_count))
        return false;

    size_t offset = byte_ptr - memory;
    return (offset % block_size) == 0;
}

void pool::free(void* ptr)
{
    if (ptr == nullptr)
        return;

    check_asserts();

    assert(owns(ptr) && "Pointer does not belong to this pool");

    free_node* node = static_cast<free_node*>(ptr);
    node->next = free_list;
    free_list = node;

    free_count++;
}

size_t pool::get_free_space() const
{
    check_asserts();
    return free_count * block_size;
}

size_t pool::get_capacity() const
{
    check_asserts();
    return capacity;
}

size_t pool::get_block_size() const
{
    return block_size;
}

size_t pool::get_block_count() const
{
    return block_count;
}

void pool::check_asserts() const
{
    assert(memory != nullptr && "Memory is nullptr. pool likely not initialized correctly.");
    assert(capacity != (size_t)-1 && "Capacity is invalid. pool likely not initialized correctly.");
    assert(free_count != (size_t)-1 && "Free count is invalid. pool likely not initialized correctly.");
    assert(block_size != (size_t)-1 && "Block size is invalid. pool likely not initialized correctly.");
    assert(block_count != (size_t)-1 && "Block count is invalid. pool likely not initialized correctly.");
}

} // namespace AL
