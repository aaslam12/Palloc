#pragma once

#include "slab.h"
#include <atomic>
#include <cstddef>
#include <mutex>

namespace AL
{
class dynamic_slab
{
public:
    explicit dynamic_slab(size_t scale = 1.0);

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
    [[nodiscard]] void* palloc(size_t size);

    // returns: nullptr if failed, else memory address (zeroed)
    [[nodiscard]] void* calloc(size_t size);

    // free pointer allocated by this dynamic_slab
    void free(void* ptr, size_t size);

    size_t get_total_capacity() const;
    size_t get_total_free() const;
    size_t get_slab_count() const;

private:
    struct slab_node
    {
        slab value;
        slab_node* next;

        slab_node(size_t scale, slab_node* next_ptr) : value(scale), next(next_ptr)
        {}
    };

    // allocate and construct a new slab_node via mmap
    slab_node* create_node(slab_node* next_ptr);

    size_t scale;
    std::atomic<slab_node*> head;
    std::atomic<size_t> node_count;
    std::mutex grow_mutex; // only held when adding a new slab
};

} // namespace AL
