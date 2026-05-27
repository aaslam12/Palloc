#pragma once

#include "palloc_atomic.h"
#include "platform.h"
#include "pool_view.h"
#include "slab_config.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <stdexcept>

namespace AL
{

struct thread_local_cache
{
    // max capacity - actual batch sizes are tuned per size class
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
    [[nodiscard]] void* palloc(size_t size);

    // returns: nullptr if failed, else the memory address of the block of memory
    // returns memory is properly aligned
    [[nodiscard]] void* calloc(size_t size);

    // NOT thread safe
    void reset();

    // decommits physical pages for all empty grown chunks across all pools
    // NOT thread-safe
    void shrink() noexcept;

    void free(void* ptr, size_t size);

    // returns true if freed successfully, false if not owned by this slab
    bool free_unsized(void* ptr);

    size_t get_pool_count() const;
    size_t get_total_capacity() const;
    size_t get_total_free() const;
    size_t get_pool_block_size(size_t index) const;
    size_t get_pool_free_space(size_t index) const;

#if PALLOC_DEBUG
    size_t get_num_comitted_blocks() const;
#endif

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
    [[nodiscard]] void* alloc_internal(size_t size);

    struct cache_entry
    {
        size_t epoch;
        slab<Tconfig>* owner;
        std::array<thread_local_cache, Tconfig::NUM_CACHED_CLASSES> storage;

        void flush()
        {
            if (!owner)
                return;

            for (size_t i = 0; i < Tconfig::NUM_CACHED_CLASSES; i++)
            {
                // flush cached pointers back to the corresponding pool_view
                auto& cache = storage[i];
                if (cache.is_empty())
                    continue;

                owner->shared_pools[i].free_batch(std::span<void*>(cache.objects.data(), cache.current));
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

    cache_entry* get_or_create_cache_entry()
    {
        assert(MAX_CACHED_SLABS != 0 && "Cannot get cached slab. Number of cached slabs is 0");

        // check the preferred slot first
        const size_t preferred = slab_id % MAX_CACHED_SLABS;
        if (caches[preferred].owner == this)
            return &caches[preferred];

        // scan for an existing entry for this slab, or the first empty slot.
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

        // found empty slot
        if (empty_slot != (size_t)-1)
        {
            cache_entry& entry = caches[empty_slot];
            entry.owner = this;
            entry.epoch = epoch.load(std::memory_order_acquire);
            init_cache_batch_sizes(entry);
            return &entry;
        }

        // all slots are occupied by other slabs.
        // evict a fixed victim slot (the last slot) and reuse it for this slab
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

    // used for TLC
    constexpr static size_t MAX_CACHED_SLABS = Tconfig::NUM_CACHED_SLABS;

    inline thread_local static std::array<cache_entry, MAX_CACHED_SLABS> caches{};

    palloc_atomic<size_t> epoch;
    std::array<pool_view, Tconfig::NUM_SIZE_CLASSES> shared_pools;

    std::byte* m_region = nullptr;
    size_t m_region_size = 0;

    // base address of each pool's payload sub-region
    std::array<std::byte*, Tconfig::NUM_SIZE_CLASSES> m_pool_bases{};
    std::array<palloc_atomic<bool>, Tconfig::NUM_SIZE_CLASSES> m_initial_committed{};

    // coarse bitmap per pool: 1 bit per chunk, 1 = committed
    std::array<palloc_atomic<uint64_t>*, Tconfig::NUM_SIZE_CLASSES> m_coarse_bitmaps{};
    std::array<size_t, Tconfig::NUM_SIZE_CLASSES> m_coarse_bitmap_bytes{};

    // per-pool fine bitmap pointer arrays
    std::array<palloc_atomic<uint64_t>**, Tconfig::NUM_SIZE_CLASSES> m_pool_chunk_bitmaps{};
    std::array<size_t, Tconfig::NUM_SIZE_CLASSES> m_num_chunks{};

    inline static palloc_atomic<size_t> next_slab_id{0};
    size_t slab_id;

    void grow_pool(size_t index) noexcept;
    bool ensure_initial_commit(size_t index) noexcept;
    void decommit_chunk(size_t index, size_t chunk_idx) noexcept;
};

template<slab_config_type Tconfig>
slab<Tconfig>::slab() : epoch(0), slab_id(next_slab_id.fetch_add(1, std::memory_order_relaxed))
{
    constexpr size_t raw_size = Tconfig::compute_total_region_size();
    size_t page_size = AL::platform_mem::page_size();
    size_t rounded_raw_size = ((raw_size + page_size - 1) / page_size) * page_size;
    size_t layout_size = 0;
    for (size_t i = 0; i < Tconfig::NUM_SIZE_CLASSES; ++i)
    {
        auto const& sc = Tconfig::SIZE_CLASS_CONFIG[i];
        size_t mask = sc.byte_size - 1;
        layout_size = (layout_size + mask) & ~mask;
        size_t pool_bytes = sc.byte_size * Tconfig::VIRTUAL_BLOCK_CEILINGS[i];
        size_t pool_bytes_rounded = ((pool_bytes + page_size - 1) / page_size) * page_size;
        layout_size += pool_bytes_rounded;
    }
    size_t desired = Tconfig::VIRTUAL_MEM_PREALLOC_SIZE;
    if (layout_size > desired)
        desired = layout_size;
    m_region_size = desired < rounded_raw_size ? rounded_raw_size : desired;

    void* mem = AL::platform_mem::virtual_alloc(m_region_size);
    if (mem == nullptr)
        throw std::runtime_error("slab virtual_alloc failed");

    m_region = static_cast<std::byte*>(mem);
    for (auto& committed : m_initial_committed)
        committed.store(false, std::memory_order_relaxed);

    // per-pool: coarse bitmap + chunk_bitmaps array + initial fine bitmap for chunk 0
    std::byte* cursor = m_region;
    for (size_t i = 0; i < Tconfig::NUM_SIZE_CLASSES; ++i)
    {
        auto& sc = Tconfig::SIZE_CLASS_CONFIG[i];

        // align cursor
        uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
        addr = (addr + sc.byte_size - 1) & ~(sc.byte_size - 1);
        cursor = reinterpret_cast<std::byte*>(addr);
        m_pool_bases[i] = cursor;

        size_t ceiling = Tconfig::VIRTUAL_BLOCK_CEILINGS[i];
        size_t chunk_size = sc.num_blocks;
        size_t num_chunks = (ceiling + chunk_size - 1) / chunk_size;
        m_num_chunks[i] = num_chunks;

        // coarse bitmap: 1 bit per chunk
        size_t coarse_words = (num_chunks + 63) / 64;
        size_t coarse_bytes = coarse_words * sizeof(uint64_t);
        auto* coarse = static_cast<palloc_atomic<uint64_t>*>(AL::platform_mem::alloc(coarse_bytes));
        if (!coarse)
        {
            // cleanup already-allocated resources then throw
            for (size_t j = 0; j < i; ++j)
            {
                AL::platform_mem::free(m_coarse_bitmaps[j], m_coarse_bitmap_bytes[j]);
                // free chunk_bitmaps[0] (initial fine bitmap)
                AL::platform_mem::free(m_pool_chunk_bitmaps[j][0], pool_view::fine_bitmap_bytes(Tconfig::SIZE_CLASS_CONFIG[j].num_blocks));
                AL::platform_mem::free(m_pool_chunk_bitmaps[j], m_num_chunks[j] * sizeof(palloc_atomic<uint64_t>*));
            }
            AL::platform_mem::free(mem, m_region_size);
            throw std::bad_alloc();
        }
        std::memset(coarse, 0, coarse_bytes);
        m_coarse_bitmaps[i] = coarse;
        m_coarse_bitmap_bytes[i] = coarse_bytes;

        // chunk_bitmaps pointer array (all null initially)
        auto** chunk_bitmaps = static_cast<palloc_atomic<uint64_t>**>(AL::platform_mem::alloc(num_chunks * sizeof(palloc_atomic<uint64_t>*)));
        if (!chunk_bitmaps)
        {
            AL::platform_mem::free(coarse, coarse_bytes);
            for (size_t j = 0; j < i; ++j)
            {
                AL::platform_mem::free(m_coarse_bitmaps[j], m_coarse_bitmap_bytes[j]);
                AL::platform_mem::free(m_pool_chunk_bitmaps[j][0], pool_view::fine_bitmap_bytes(Tconfig::SIZE_CLASS_CONFIG[j].num_blocks));
                AL::platform_mem::free(m_pool_chunk_bitmaps[j], m_num_chunks[j] * sizeof(palloc_atomic<uint64_t>*));
            }
            AL::platform_mem::free(mem, m_region_size);
            throw std::bad_alloc();
        }
        std::memset(chunk_bitmaps, 0, num_chunks * sizeof(palloc_atomic<uint64_t>*));
        m_pool_chunk_bitmaps[i] = chunk_bitmaps;

        // fine bitmap for chunk 0 (initial commit)
        size_t fine_bytes = pool_view::fine_bitmap_bytes(chunk_size);
        auto* fine = static_cast<palloc_atomic<uint64_t>*>(AL::platform_mem::alloc(fine_bytes));
        if (!fine)
        {
            AL::platform_mem::free(coarse, coarse_bytes);
            AL::platform_mem::free(chunk_bitmaps, num_chunks * sizeof(palloc_atomic<uint64_t>*));
            for (size_t j = 0; j < i; ++j)
            {
                AL::platform_mem::free(m_coarse_bitmaps[j], m_coarse_bitmap_bytes[j]);
                AL::platform_mem::free(m_pool_chunk_bitmaps[j][0], pool_view::fine_bitmap_bytes(Tconfig::SIZE_CLASS_CONFIG[j].num_blocks));
                AL::platform_mem::free(m_pool_chunk_bitmaps[j], m_num_chunks[j] * sizeof(palloc_atomic<uint64_t>*));
            }
            AL::platform_mem::free(mem, m_region_size);
            throw std::bad_alloc();
        }
        std::memset(fine, 0, fine_bytes);
        // mark tail bits
        size_t tail = chunk_size % 64;
        size_t words = (chunk_size + 63) / 64;
        if (tail)
            fine[words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);
        chunk_bitmaps[0] = fine;

        // mark chunk 0 as committed in coarse bitmap
        coarse[0].fetch_or(uint64_t(1), std::memory_order_relaxed);

        shared_pools[i].init_from_region(cursor, chunk_bitmaps, sc.byte_size, chunk_size, ceiling, chunk_size);
        size_t pool_bytes = sc.byte_size * ceiling;
        size_t pool_bytes_rounded = ((pool_bytes + page_size - 1) / page_size) * page_size;
        cursor += pool_bytes_rounded;
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

    // munmap the single contiguous region backing all pool_views
    if (m_region != nullptr)
    {
        AL::platform_mem::free(m_region, m_region_size);
        m_region = nullptr;
    }

    // free all bitmaps
    for (size_t i = 0; i < Tconfig::NUM_SIZE_CLASSES; ++i)
    {
        if (m_pool_chunk_bitmaps[i])
        {
            size_t fine_bytes = pool_view::fine_bitmap_bytes(Tconfig::SIZE_CLASS_CONFIG[i].num_blocks);
            for (size_t c = 0; c < m_num_chunks[i]; ++c)
            {
                if (m_pool_chunk_bitmaps[i][c])
                    AL::platform_mem::free(m_pool_chunk_bitmaps[i][c], fine_bytes);
            }
            AL::platform_mem::free(m_pool_chunk_bitmaps[i], m_num_chunks[i] * sizeof(palloc_atomic<uint64_t>*));
        }
        if (m_coarse_bitmaps[i])
            AL::platform_mem::free(m_coarse_bitmaps[i], m_coarse_bitmap_bytes[i]);
    }
}

template<slab_config_type Tconfig>
void* slab<Tconfig>::palloc(size_t size)
{
    if (size == 0 || size == (size_t)-1) [[unlikely]]
        return nullptr;
    if (Tconfig::SIZE_CLASS_CONFIG[Tconfig::NUM_SIZE_CLASSES - 1].byte_size < size) [[unlikely]]
        return nullptr;

    size_t index = size_to_index(size);
    if (index == static_cast<size_t>(-1)) [[unlikely]]
        return nullptr;

    void* ptr = alloc_internal(size);
    if (ptr != nullptr) [[likely]]
        return ptr;

    // pool exhausted - attempt to grow
    grow_pool(index);
    return alloc_internal(size);
}

template<slab_config_type Tconfig>
void slab<Tconfig>::grow_pool(size_t index) noexcept
{
    pool_view& p = shared_pools[index];
    if (!ensure_initial_commit(index)) [[unlikely]]
        return;

    if (p.committed_blocks() >= p.virtual_block_ceiling())
        return;

    size_t old = 0;
    if (!p.try_reserve_chunk(old))
        return; // another thread won the CAS - caller retries alloc

    size_t block_size = Tconfig::SIZE_CLASS_CONFIG[index].byte_size;
    size_t chunk_blocks = p.blocks_per_chunk();
    size_t chunk_idx = old / chunk_blocks;

    // commit payload pages
    std::byte* chunk_base = m_pool_bases[index] + old * block_size;
    AL::platform_mem::virtual_commit(chunk_base, chunk_blocks * block_size);

    // allocate and zero fine bitmap for this chunk
    size_t fine_bytes = pool_view::fine_bitmap_bytes(chunk_blocks);
    auto* fine = static_cast<palloc_atomic<uint64_t>*>(AL::platform_mem::alloc(fine_bytes));
    if (fine)
    {
        std::memset(fine, 0, fine_bytes);
        size_t tail = chunk_blocks % 64;
        size_t words = (chunk_blocks + 63) / 64;
        if (tail)
            fine[words - 1].store(~uint64_t(0) << tail, std::memory_order_relaxed);
        // release-store the bitmap pointer so readers see fully-initialized bitmap
        std::atomic_ref<palloc_atomic<uint64_t>*>(m_pool_chunk_bitmaps[index][chunk_idx])
            .store(fine, std::memory_order_release);

        // set coarse bit
        size_t cw = chunk_idx / 64, cb = chunk_idx % 64;
        m_coarse_bitmaps[index][cw].fetch_or(uint64_t(1) << cb, std::memory_order_relaxed);
    }

    // make new blocks visible
    p.advance_committed(old + chunk_blocks);
}

template<slab_config_type Tconfig>
bool slab<Tconfig>::ensure_initial_commit(size_t index) noexcept
{
    if (m_initial_committed[index].load(std::memory_order_acquire))
        return true;

    size_t block_size = Tconfig::SIZE_CLASS_CONFIG[index].byte_size;
    size_t chunk_blocks = Tconfig::SIZE_CLASS_CONFIG[index].num_blocks;
    size_t page_size = AL::platform_mem::page_size();
    size_t commit_bytes = block_size * chunk_blocks;
    size_t commit_bytes_rounded = ((commit_bytes + page_size - 1) / page_size) * page_size;
    if (!AL::platform_mem::virtual_commit(m_pool_bases[index], commit_bytes_rounded))
        return false;

    m_initial_committed[index].store(true, std::memory_order_release);
    return true;
}

template<slab_config_type Tconfig>
void slab<Tconfig>::decommit_chunk(size_t index, size_t chunk_idx) noexcept // chunk_idx (null = not committed)
{
    pool_view& p = shared_pools[index];
    size_t block_size = Tconfig::SIZE_CLASS_CONFIG[index].byte_size;
    size_t chunk_blocks = p.blocks_per_chunk();

    // decommit payload
    std::byte* chunk_base = m_pool_bases[index] + chunk_idx * chunk_blocks * block_size;
    AL::platform_mem::virtual_free(chunk_base, chunk_blocks * block_size);

    // free fine bitmap and null the pointer
    size_t fine_bytes = pool_view::fine_bitmap_bytes(chunk_blocks);
    if (m_pool_chunk_bitmaps[index][chunk_idx])
    {
        AL::platform_mem::free(m_pool_chunk_bitmaps[index][chunk_idx], fine_bytes);
        m_pool_chunk_bitmaps[index][chunk_idx] = nullptr;
    }

    // clear coarse bit
    size_t cw = chunk_idx / 64, cb = chunk_idx % 64;
    m_coarse_bitmaps[index][cw].fetch_and(~(uint64_t(1) << cb), std::memory_order_relaxed);
}

template<slab_config_type Tconfig>
void* slab<Tconfig>::alloc_internal(size_t size)
{
    size_t index = size_to_index(size);
    if (index == (size_t)-1) [[unlikely]]
        return nullptr;

    pool_view& p = shared_pools[index];
    if (!ensure_initial_commit(index)) [[unlikely]]
        return nullptr;

    // if is part of the TLC
    if (index < Tconfig::NUM_CACHED_CLASSES) [[likely]]
    {
        cache_entry* cached_entry = get_or_create_cache_entry();
        thread_local_cache& cache = cached_entry->storage[index];
        size_t current_epoch = epoch.load(std::memory_order_acquire);
        if (cached_entry->epoch != current_epoch) [[unlikely]]
        {
            cached_entry->invalidate_all();
            cached_entry->epoch = current_epoch;
        }

        if (auto elem = cache.try_pop()) [[likely]]
            return elem;

        size_t num_allocated = p.alloc_batch(cache.batch_size, cache.objects.data());
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
    void* ptr = palloc(size);
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
void slab<Tconfig>::shrink() noexcept
{
    for (size_t i = 0; i < Tconfig::NUM_SIZE_CLASSES; ++i)
    {
        pool_view& p = shared_pools[i];
        size_t committed = p.committed_blocks();
        size_t chunk_size = p.blocks_per_chunk();

        if (committed <= chunk_size)
            continue; // only the initial chunk - nothing grown to decommit

        size_t num_chunks = committed / chunk_size;

        // scan grown chunks top-down (skip chunk 0 = initial commit)
        // find the highest contiguous run of empty chunks and trim committed_blocks
        size_t new_committed = committed;
        for (size_t c = num_chunks; c-- > 1;)
        {
            if (!p.is_chunk_empty(c))
                break; // stop at first non-empty chunk from the top
            decommit_chunk(i, c);
            new_committed = c * chunk_size;
        }

        if (new_committed < committed)
            p.decommit_blocks(new_committed);
    }
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

    pool_view& p = shared_pools[index];
    if (index < Tconfig::NUM_CACHED_CLASSES) [[likely]]
    {
        auto cached_entry = get_or_create_cache_entry();
        thread_local_cache& cache = cached_entry->storage[index];
        size_t current_epoch = epoch.load(std::memory_order_acquire);
        if (cached_entry->epoch != current_epoch) [[unlikely]]
        {
            cached_entry->invalidate_all();
            cached_entry->epoch = current_epoch;
        }

        if (cache.is_full()) [[unlikely]]
        {
            // flush tail segment first, keep recent entries hot in TLC
            auto flush_span = std::span<void*>(cache.objects.data() + (cache.current - cache.batch_size), cache.batch_size);
            p.free_batch(flush_span);
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
        pool_view& p = shared_pools[i];
        if (p.owns(ptr))
        {
            if (i < Tconfig::NUM_CACHED_CLASSES) [[likely]]
            {
                auto cached_entry = get_or_create_cache_entry();
                thread_local_cache& cache = cached_entry->storage[i];
                size_t current_epoch = epoch.load(std::memory_order_acquire);
                if (cached_entry->epoch != current_epoch) [[unlikely]]
                {
                    cached_entry->invalidate_all();
                    cached_entry->epoch = current_epoch;
                }

                if (cache.is_full()) [[unlikely]]
                {
                    // flush tail segment first, keep recent entries hot in TLC
                    p.free_batch(std::span<void*>(cache.objects.data() + (cache.current - cache.batch_size), cache.batch_size));
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
        total += p.capacity();
    return total;
}

template<slab_config_type Tconfig>
size_t slab<Tconfig>::get_total_free() const
{
    size_t total = 0;
    for (const auto& p : shared_pools)
        total += p.free_count() * p.block_size();
    return total;
}

template<slab_config_type Tconfig>
size_t slab<Tconfig>::get_pool_block_size(size_t index) const
{
    if (index >= Tconfig::NUM_SIZE_CLASSES)
        return 0;
    return shared_pools[index].block_size();
}

template<slab_config_type Tconfig>
size_t slab<Tconfig>::get_pool_free_space(size_t index) const
{
    if (index >= Tconfig::NUM_SIZE_CLASSES)
        return 0;
    return shared_pools[index].free_count() * shared_pools[index].block_size();
}

#if PALLOC_DEBUG
template<slab_config_type Tconfig>
size_t slab<Tconfig>::get_num_comitted_blocks() const
{
    return 0;
}
#endif

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
