#pragma once

#include "palloc_atomic.h"
#include "platform.h"
#include "pool.h"
#include "slab_config.h"
#include <cassert>
#include <cstddef>
#include <cstring>
#include <new>
#include <stdexcept>

namespace AL
{

struct thread_local_cache
{
    // max capacity — actual batch sizes are tuned per size class
    static constexpr size_t object_count = 128;

    std::array<void*, object_count> objects;
    size_t current = 0;
    size_t batch_size = object_count / 2; // filled by slab on cache init

    [[nodiscard]] void* try_pop()
    {
        if (is_empty())
            return nullptr;

        current--;
        return objects[current];
    }

    void push(void* ptr)
    {
        assert(!is_full() && "Thread local cache is full");

        objects[current] = ptr;
        current++;
    }

    bool is_empty() const
    {
        return current == 0;
    }

    bool is_full() const
    {
        return current == object_count;
    }

    void invalidate()
    {
        current = 0;
    }
};

template<slab_config_type Tconfig>
class slab
{
public:
    slab();
    ~slab();

    slab(const slab&) = delete;
    slab& operator=(const slab&) = delete;
    slab(slab&&) noexcept = delete;
    slab& operator=(slab&&) noexcept = delete;

    // returns: nullptr if failed, else the memory address of the block of memory
    // returns memory is properly aligned
    [[nodiscard]] void* alloc(size_t size);

    // returns: nullptr if failed, else the memory address of the block of memory
    // returns memory is properly aligned
    [[nodiscard]] void* calloc(size_t size);

    // NOT thread safe
    // returns: -1 if failed
    void reset();

    // returns: -1 if failed
    void free(void* ptr, size_t size);

    // returns true if freed successfully, false if not owned by this slab
    bool free_unsized(void* ptr);

    size_t get_pool_count() const;
    size_t get_total_capacity() const;
    size_t get_total_free() const;
    size_t get_pool_block_size(size_t index) const;
    size_t get_pool_free_space(size_t index) const;

    // check if pointer belongs to this slab
    bool owns(void* ptr) const;

    // get the contiguous memory region backing all pools
    std::byte* region_start() const
    {
        return m_region;
    }
    std::byte* region_end() const
    {
        return m_region + m_region_size;
    }

    static constexpr size_t size_to_index(size_t size)
    {
        if (size == 0 || size > Tconfig::SIZE_CLASS_CONFIG[Tconfig::NUM_SIZE_CLASSES - 1].byte_size)
            return static_cast<size_t>(-1);
        size_t s = size < Tconfig::SIZE_CLASS_CONFIG[0].byte_size ? Tconfig::SIZE_CLASS_CONFIG[0].byte_size : size;
        size_t vi = std::bit_width(std::bit_ceil(s)) - std::bit_width(Tconfig::SIZE_CLASS_CONFIG[0].byte_size);
        if (vi >= Tconfig::INDEX_SPAN)
            return static_cast<size_t>(-1);
        return Tconfig::INDEX_LUT[vi];
    }

    static constexpr size_t index_to_size_class(size_t index)
    {
        if (index >= Tconfig::NUM_SIZE_CLASSES)
            return 0;
        return Tconfig::SIZE_CLASS_CONFIG[index].byte_size;
    }

private:
    constexpr static size_t MAX_CACHED_SLABS = 4;

    struct cache_entry
    {
        size_t epoch;
        slab<Tconfig>* owner;
        std::array<thread_local_cache, Tconfig::NUM_CACHED_CLASSES> storage;

        void flush()
        {
            if (!owner)
                return; // should we assert?

            for (size_t i = 0; i < Tconfig::NUM_CACHED_CLASSES; i++)
            {
                // move pointers to appropriate pool from storage?
                auto& cache = storage[i];
                if (cache.is_empty())
                    continue;

                owner->shared_pools[i].free_batched_internal(cache.current, cache.objects.data());
                cache.current = 0;
            }
        }

        void invalidate_all()
        {
            if (!owner)
                return;

            for (size_t i = 0; i < Tconfig::NUM_CACHED_CLASSES; i++)
                storage[i].invalidate();
        }
    };

    inline thread_local static std::array<cache_entry, MAX_CACHED_SLABS> caches{};

    cache_entry* get_cached_slab()
    {
        assert(MAX_CACHED_SLABS != 0 && "Cannot get cached slab. Number of cached slabs is 0");

        // O(1) fast path: check the preferred hash slot first
        const size_t preferred = slab_id % MAX_CACHED_SLABS;
        if (caches[preferred].owner == this)
            return &caches[preferred];

        // Scan for an existing entry for this slab, or the first empty slot.
        // Slabs with colliding hash IDs will land in different slots when space is available.
        size_t empty_slot = caches[preferred].owner == nullptr ? preferred : (size_t)-1;
        for (size_t i = 0; i < MAX_CACHED_SLABS; ++i)
        {
            if (i == preferred)
                continue;
            if (caches[i].owner == this)
                return &caches[i];
            if (caches[i].owner == nullptr && empty_slot == (size_t)-1)
                empty_slot = i;
        }

        // Claim an empty slot (prefer the hash slot to keep affinity for next time)
        if (empty_slot != (size_t)-1)
        {
            cache_entry& entry = caches[empty_slot];
            entry.owner = this;
            entry.epoch = epoch.load(std::memory_order_acquire);
            init_cache_batch_sizes(entry);
            return &entry;
        }

        // All slots occupied by other slabs.
        // Evict the last slot (not the preferred one) so that slabs whose preferred
        // slots are 0..MAX_CACHED_SLABS-2 remain stable across round-robin cycling.
        // This mirrors LRU-ish eviction: the last slot acts as the "victim" slot.
        cache_entry& entry = caches[MAX_CACHED_SLABS - 1];
        entry.flush();
        entry.owner = this;
        entry.epoch = epoch.load(std::memory_order_acquire);
        init_cache_batch_sizes(entry);
        return &entry;
    }

    static void init_cache_batch_sizes(cache_entry& entry)
    {
        for (size_t i = 0; i < Tconfig::NUM_CACHED_CLASSES; ++i)
            entry.storage[i].batch_size = Tconfig::SIZE_CLASS_CONFIG[i].batch_size;
    }

    palloc_atomic<size_t> epoch;
    std::array<pool, Tconfig::NUM_SIZE_CLASSES> shared_pools;

    std::byte* m_region = nullptr;
    size_t m_region_size = 0; // total size of the contiguous region backing all pools in raw bytes

    inline static palloc_atomic<size_t> next_slab_id{0};
    size_t slab_id;
};

template<slab_config_type Tconfig>
slab<Tconfig>::slab() : epoch(0), slab_id(next_slab_id.fetch_add(1, std::memory_order_relaxed))
{
    constexpr size_t raw_size = Tconfig::compute_total_region_size();
    size_t page_size = AL::platform_mem::page_size();
    size_t rounded_raw_size = ((raw_size + page_size - 1) / page_size) * page_size;
    m_region_size = Tconfig::VIRTUAL_MEM_PREALLOC_SIZE < rounded_raw_size ? rounded_raw_size : Tconfig::VIRTUAL_MEM_PREALLOC_SIZE;

    void* mem = AL::platform_mem::virtual_alloc(m_region_size);
    if (mem == nullptr)
        throw std::runtime_error("slab virtual_alloc failed: required huge-page-backed virtual reservation is unavailable");

    m_region = static_cast<std::byte*>(mem);

    // carve sub-regions for each pool
    std::byte* cursor = m_region;
    for (size_t i = 0; i < Tconfig::NUM_SIZE_CLASSES; ++i)
    {
        auto& sc = Tconfig::SIZE_CLASS_CONFIG[i];
        // align cursor to block_size
        auto addr = reinterpret_cast<uintptr_t>(cursor);
        uintptr_t mask = sc.byte_size - 1;
        addr = (addr + mask) & ~mask;
        cursor = reinterpret_cast<std::byte*>(addr);

        shared_pools[i].init_from_region(cursor, sc.byte_size, sc.num_blocks);
        cursor += pool_view::required_region_size(sc.byte_size, sc.num_blocks);
    }
}

template<slab_config_type Tconfig>
slab<Tconfig>::~slab()
{
    // invalidate TLC entries for this slab
    const size_t preferred = slab_id % MAX_CACHED_SLABS;
    if (caches[preferred].owner == this)
    {
        caches[preferred].invalidate_all();
        caches[preferred].owner = nullptr;
    }
    else
    {
        for (size_t i = 0; i < MAX_CACHED_SLABS; ++i)
        {
            if (i == preferred)
                continue;
            if (caches[i].owner == this)
            {
                caches[i].invalidate_all();
                caches[i].owner = nullptr;
                break;
            }
        }
    }

    // munmap the single contiguous region (pools are non-owning, their destructors are no-ops)
    if (m_region != nullptr)
    {
        AL::platform_mem::free(m_region, m_region_size);
        m_region = nullptr;
    }
}

template<slab_config_type Tconfig>
void* slab<Tconfig>::alloc(size_t size)
{
    if (size == 0 || size == (size_t)-1) [[unlikely]]
        return nullptr;
    if (Tconfig::SIZE_CLASS_CONFIG[Tconfig::NUM_SIZE_CLASSES - 1].byte_size < size) [[unlikely]]
        return nullptr;

    size_t index = size_to_index(size);
    if (index == (size_t)-1) [[unlikely]]
        return nullptr;

    pool& p = shared_pools[index];

    if (index < Tconfig::NUM_CACHED_CLASSES) [[likely]]
    {
        auto cached_entry = get_cached_slab();
        thread_local_cache& cache = cached_entry->storage[index];
        size_t current_epoch = epoch.load(std::memory_order_acquire);
        if (cached_entry->epoch != current_epoch) [[unlikely]]
        {
            cached_entry->invalidate_all();
            cached_entry->epoch = current_epoch;
        }

        if (auto elem = cache.try_pop()) [[likely]]
            return elem;

        size_t num_allocated = p.alloc_batched_internal(cache.batch_size, cache.objects.data());
        cache.current = num_allocated;
        return cache.try_pop();
    }
    else
    {
        return p.alloc();
    }
}

template<slab_config_type Tconfig>
void* slab<Tconfig>::calloc(size_t size)
{
    void* ptr = alloc(size);
    if (ptr != nullptr)
    {
        size_t actual_size = Tconfig::SIZE_CLASS_CONFIG[size_to_index(size)].byte_size;
        std::memset(ptr, 0, actual_size);
    }
    return ptr;
}

template<slab_config_type Tconfig>
void slab<Tconfig>::reset()
{
    for (auto& p : shared_pools)
        p.reset();
    epoch.fetch_add(1, std::memory_order_release);
}

template<slab_config_type Tconfig>
void slab<Tconfig>::free(void* ptr, size_t size)
{
    if (size == 0 || size == (size_t)-1) [[unlikely]]
        return;
    if (Tconfig::SIZE_CLASS_CONFIG[Tconfig::NUM_SIZE_CLASSES - 1].byte_size < size) [[unlikely]]
        return;

    size_t index = size_to_index(size);
    if (index == (size_t)-1) [[unlikely]]
        return;

    pool& p = shared_pools[index];
    if (index < Tconfig::NUM_CACHED_CLASSES) [[likely]]
    {
        auto cached_entry = get_cached_slab();
        thread_local_cache& cache = cached_entry->storage[index];
        size_t current_epoch = epoch.load(std::memory_order_acquire);
        if (cached_entry->epoch != current_epoch) [[unlikely]]
        {
            cached_entry->invalidate_all();
            cached_entry->epoch = current_epoch;
        }

        if (cache.is_full()) [[unlikely]]
        {
            p.free_batched_internal(cache.batch_size, cache.objects.data() + (cache.current - cache.batch_size));
            cache.current -= cache.batch_size;
        }
        cache.push(ptr);
    }
    else
    {
        shared_pools[index].free(ptr);
    }
}

template<slab_config_type Tconfig>
bool slab<Tconfig>::free_unsized(void* ptr)
{
    for (size_t i = 0; i < Tconfig::NUM_SIZE_CLASSES; ++i)
    {
        pool& p = shared_pools[i];
        if (p.owns(ptr))
        {
            if (i < Tconfig::NUM_CACHED_CLASSES) [[likely]]
            {
                auto cached_entry = get_cached_slab();
                thread_local_cache& cache = cached_entry->storage[i];
                size_t current_epoch = epoch.load(std::memory_order_acquire);
                if (cached_entry->epoch != current_epoch) [[unlikely]]
                {
                    cached_entry->invalidate_all();
                    cached_entry->epoch = current_epoch;
                }

                if (cache.is_full()) [[unlikely]]
                {
                    p.free_batched_internal(cache.batch_size, cache.objects.data() + (cache.current - cache.batch_size));
                    cache.current -= cache.batch_size;
                }
                cache.push(ptr);
            }
            else
            {
                p.free(ptr);
            }
            return true;
        }
    }
    return false;
}

template<slab_config_type Tconfig>
size_t slab<Tconfig>::get_pool_count() const
{
    return std::size(shared_pools);
}

template<slab_config_type Tconfig>
size_t slab<Tconfig>::get_total_capacity() const
{
    size_t total = 0;
    for (const auto& p : shared_pools)
        total += p.get_capacity();
    return total;
}

template<slab_config_type Tconfig>
size_t slab<Tconfig>::get_total_free() const
{
    size_t total = 0;
    for (const auto& p : shared_pools)
        total += p.get_free_space();
    return total;
}

template<slab_config_type Tconfig>
size_t slab<Tconfig>::get_pool_block_size(size_t index) const
{
    if (index >= Tconfig::NUM_SIZE_CLASSES)
        return 0;
    return shared_pools[index].get_block_size();
}

template<slab_config_type Tconfig>
size_t slab<Tconfig>::get_pool_free_space(size_t index) const
{
    if (index >= Tconfig::NUM_SIZE_CLASSES)
        return 0;
    return shared_pools[index].get_free_space();
}

template<slab_config_type Tconfig>
bool slab<Tconfig>::owns(void* ptr) const
{
    for (const auto& p : shared_pools)
        if (p.owns(ptr))
            return true;
    return false;
}

using default_slab = slab<slab_config<>>;

} // namespace AL
