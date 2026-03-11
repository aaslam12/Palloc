#pragma once

#include "platform.h"
#include "slab.h"
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>

namespace AL
{

template<typename Tconfig>
class dynamic_slab
{
public:
    explicit dynamic_slab();

    // WARNING: this destructor only cleans up the current thread's thread local caches (TLC).
    // if other threads have allocated from this dynamic_slab, their TLC
    // will still hold pointers to slabs managed by this object.
    // ensure all other threads have ceased operations or cleared their caches before destroying this object
    ~dynamic_slab();

    dynamic_slab(const dynamic_slab&) = delete;
    dynamic_slab& operator=(const dynamic_slab&) = delete;
    dynamic_slab(dynamic_slab&&) = delete;
    dynamic_slab& operator=(dynamic_slab&&) = delete;

    // returns: nullptr if failed, else memory address
    // returns memory is properly aligned
    [[nodiscard]] void* palloc(size_t size);

    // returns: nullptr if failed, else memory address (zeroed)
    // returns memory is properly aligned
    [[nodiscard]] void* calloc(size_t size);

    // free pointer allocated by this dynamic_slab
    void free(void* ptr, size_t size);

    size_t get_total_capacity() const;
    size_t get_total_free() const;
    size_t get_slab_count() const;

private:
    struct slab_node
    {
        slab<Tconfig> value;
        slab_node* next;

        slab_node(slab_node* next_ptr) : value(), next(next_ptr)
        {}
    };

    // allocate and construct a new slab_node via mmap
    slab_node* create_node(slab_node* next_ptr);

    std::atomic<slab_node*> head;
    std::atomic<size_t> node_count;
    std::mutex grow_mutex; // only held when adding a new slab
};

template<typename Tconfig>
typename dynamic_slab<Tconfig>::slab_node* dynamic_slab<Tconfig>::create_node(slab_node* next_ptr)
{
    void* mem = AL::platform_mem::alloc(sizeof(slab_node));
    if (mem == nullptr)
        return nullptr;

    try
    {
        // uses placement new. initializes the object at the given address 'mem'.
        // this acts as a constructor call on existing memory and does NOT allocate new memory.
        return std::construct_at(static_cast<slab_node*>(mem), next_ptr);
    }
    catch (...)
    {
        AL::platform_mem::free(mem, sizeof(slab_node));
        return nullptr;
    }
}

template<typename Tconfig>
dynamic_slab<Tconfig>::dynamic_slab() : head(nullptr), node_count(0)
{
    slab_node* node = create_node(nullptr);
    if (node)
    {
        head.store(node, std::memory_order_release);
        node_count.store(1, std::memory_order_relaxed);
    }
}

template<typename Tconfig>
dynamic_slab<Tconfig>::~dynamic_slab()
{
    slab_node* current = head.load(std::memory_order_acquire);
    while (current)
    {
        slab_node* next = current->next;
        current->~slab_node();
        AL::platform_mem::free(current, sizeof(slab_node));
        current = next;
    }
}

template<typename Tconfig>
void* dynamic_slab<Tconfig>::palloc(size_t size)
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

template<typename Tconfig>
void* dynamic_slab<Tconfig>::calloc(size_t size)
{
    void* ptr = palloc(size);
    if (ptr)
    {
        size_t index = slab<Tconfig>::size_to_index(size);
        if (index != static_cast<size_t>(-1))
            std::memset(ptr, 0, slab<Tconfig>::index_to_size_class(index));
    }
    return ptr;
}

template<typename Tconfig>
void dynamic_slab<Tconfig>::free(void* ptr, size_t size)
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

template<typename Tconfig>
size_t dynamic_slab<Tconfig>::get_total_capacity() const
{
    size_t total = 0;
    for (slab_node* node = head.load(std::memory_order_acquire); node; node = node->next)
        total += node->value.get_total_capacity();
    return total;
}

template<typename Tconfig>
size_t dynamic_slab<Tconfig>::get_total_free() const
{
    size_t total = 0;
    for (slab_node* node = head.load(std::memory_order_acquire); node; node = node->next)
        total += node->value.get_total_free();
    return total;
}

template<typename Tconfig>
size_t dynamic_slab<Tconfig>::get_slab_count() const
{
    return node_count.load(std::memory_order_relaxed);
}

using default_dynamic_slab = dynamic_slab<slab_config<>>;

} // namespace AL
