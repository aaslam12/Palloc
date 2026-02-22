#include "dynamic_slab.h"
#include <cstring>
#include <memory>
#include <sys/mman.h>

namespace AL
{

dynamic_slab::slab_node* dynamic_slab::create_node(slab_node* next_ptr)
{
    void* mem = mmap(nullptr, sizeof(slab_node), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return nullptr;

    try
    {
        // uses placement new. initializes the object at the given address 'mem'.
        // this acts as a constructor call on existing memory and does NOT allocate new memory.
        return std::construct_at(static_cast<slab_node*>(mem), scale, next_ptr);
    }
    catch (...)
    {
        munmap(mem, sizeof(slab_node));
        return nullptr;
    }
}

dynamic_slab::dynamic_slab(double s) : scale(s), head(nullptr), node_count(0)
{
    slab_node* node = create_node(nullptr);
    if (node)
    {
        head.store(node, std::memory_order_release);
        node_count.store(1, std::memory_order_relaxed);
    }
}

dynamic_slab::~dynamic_slab()
{
    slab_node* current = head.load(std::memory_order_acquire);
    while (current)
    {
        slab_node* next = current->next;
        current->~slab_node();
        munmap(current, sizeof(slab_node));
        current = next;
    }
}

void* dynamic_slab::palloc(size_t size)
{
    if (size == 0 || size == static_cast<size_t>(-1))
        return nullptr;

    // lock free traversal
    // nodes are only prepended, never removed
    for (slab_node* node = head.load(std::memory_order_acquire); node; node = node->next)
    {
        void* p = node->value.alloc(size);
        if (p)
            return p;
    }

    // all slabs exhausted — grow under lock
    std::lock_guard<std::mutex> lock(grow_mutex);

    // double check if another thread may have grown while we waited
    for (slab_node* node = head.load(std::memory_order_acquire); node; node = node->next)
    {
        void* p = node->value.alloc(size);
        if (p)
            return p;
    }

    slab_node* new_node = create_node(head.load(std::memory_order_relaxed));
    if (!new_node)
        return nullptr;

    head.store(new_node, std::memory_order_release);
    node_count.fetch_add(1, std::memory_order_relaxed);

    return new_node->value.alloc(size);
}

void* dynamic_slab::calloc(size_t size)
{
    void* ptr = palloc(size);
    if (ptr)
    {
        size_t index = slab::size_to_index(size);
        if (index != static_cast<size_t>(-1))
            std::memset(ptr, 0, slab::index_to_size_class(index));
    }
    return ptr;
}

void dynamic_slab::free(void* ptr, size_t size)
{
    if (ptr == nullptr || size == 0 || size == static_cast<size_t>(-1))
        return;

    // lock free traversal
    // find owning slab, then call its thread-safe free
    for (slab_node* node = head.load(std::memory_order_acquire); node; node = node->next)
    {
        if (node->value.owns(ptr))
        {
            node->value.free(ptr, size);
            return;
        }
    }
}

size_t dynamic_slab::get_total_capacity() const
{
    size_t total = 0;
    for (slab_node* node = head.load(std::memory_order_acquire); node; node = node->next)
        total += node->value.get_total_capacity();
    return total;
}

size_t dynamic_slab::get_total_free() const
{
    size_t total = 0;
    for (slab_node* node = head.load(std::memory_order_acquire); node; node = node->next)
        total += node->value.get_total_free();
    return total;
}

size_t dynamic_slab::get_slab_count() const
{
    return node_count.load(std::memory_order_relaxed);
}

} // namespace AL
