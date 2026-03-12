#include "pool_view.h"
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <set>
#include <vector>

// helper: allocate a properly-aligned buffer and init a pool_view
static std::vector<std::byte> make_region(size_t block_size, size_t block_count)
{
    size_t needed = AL::pool_view::required_region_size(block_size, block_count);
    // over-allocate + align to block_size (vector guarantees >= 16-byte alignment,
    // but we need alignof(uint64_t) at minimum which is 8 on all targets)
    std::vector<std::byte> buf(needed + block_size);
    return buf;
}

static void* aligned_base(std::vector<std::byte>& buf, size_t alignment)
{
    void* ptr = buf.data();
    size_t space = buf.size();
    return std::align(alignment, 1, ptr, space);
}

// Convenience: align to block_size (the required minimum for pool_view)
static void* block_aligned_base(std::vector<std::byte>& buf, size_t block_size)
{
    return aligned_base(buf, block_size);
}

TEST_CASE("pool_view: required_region_size", "[pool_view]")
{
    // 10 blocks of 64 bytes: bitmap = ceil(10/64) = 1 word = 8 bytes
    // aligned offset = ceil(8/64)*64 = 64, payload = 10*64 = 640
    REQUIRE(AL::pool_view::required_region_size(64, 10) == 64 + 640);

    // 64 blocks of 8 bytes: bitmap = 1 word = 8 bytes
    // aligned offset = ceil(8/8)*8 = 8, payload = 64*8 = 512
    REQUIRE(AL::pool_view::required_region_size(8, 64) == 8 + 512);

    // 65 blocks of 8 bytes: bitmap = 2 words = 16 bytes
    // aligned offset = ceil(16/8)*8 = 16, payload = 65*8 = 520
    REQUIRE(AL::pool_view::required_region_size(8, 65) == 16 + 520);

    // 128 blocks of 32 bytes: bitmap = 2 words = 16 bytes
    // aligned offset = ceil(16/32)*32 = 32, payload = 128*32 = 4096
    REQUIRE(AL::pool_view::required_region_size(32, 128) == 32 + 4096);
}

TEST_CASE("pool_view: init and diagnostics", "[pool_view]")
{
    auto buf = make_region(64, 10);
    void* base = block_aligned_base(buf, 64);
    REQUIRE(base != nullptr);

    AL::pool_view view;
    REQUIRE_FALSE(view.is_initialized());

    view.init_from_region(base, 64, 10);

    REQUIRE(view.is_initialized());
    REQUIRE(view.block_size() == 64);
    REQUIRE(view.block_count() == 10);
    REQUIRE(view.free_count() == 10);
    REQUIRE(view.capacity() == 640);
    REQUIRE(view.memory_start() != nullptr);
    REQUIRE(view.memory_end() == view.memory_start() + 640);
}

TEST_CASE("pool_view: single alloc and free", "[pool_view]")
{
    auto buf = make_region(64, 10);
    void* base = block_aligned_base(buf, 64);
    AL::pool_view view;
    view.init_from_region(base, 64, 10);

    void* ptr = view.alloc();
    REQUIRE(ptr != nullptr);
    REQUIRE(view.free_count() == 9);
    REQUIRE(view.owns(ptr));

    // alignment check
    REQUIRE(reinterpret_cast<uintptr_t>(ptr) % 64 == 0);

    view.free(ptr);
    REQUIRE(view.free_count() == 10);
}

TEST_CASE("pool_view: freed block can be reallocated", "[pool_view]")
{
    auto buf = make_region(64, 10);
    void* base = block_aligned_base(buf, 64);
    AL::pool_view view;
    view.init_from_region(base, 64, 10);

    void* p1 = view.alloc();
    view.free(p1);
    void* p2 = view.alloc();
    REQUIRE(p2 == p1);
}

TEST_CASE("pool_view: allocate all blocks then exhaust", "[pool_view]")
{
    auto buf = make_region(64, 10);
    void* base = block_aligned_base(buf, 64);
    AL::pool_view view;
    view.init_from_region(base, 64, 10);

    std::set<void*> ptrs;
    for (size_t i = 0; i < 10; ++i)
    {
        void* ptr = view.alloc();
        REQUIRE(ptr != nullptr);
        ptrs.insert(ptr);
    }
    REQUIRE(ptrs.size() == 10);
    REQUIRE(view.free_count() == 0);
    REQUIRE(view.alloc() == nullptr);
}

TEST_CASE("pool_view: calloc zeros memory", "[pool_view]")
{
    auto buf = make_region(128, 5);
    void* base = block_aligned_base(buf, 128);
    AL::pool_view view;
    view.init_from_region(base, 128, 5);

    // dirty the memory
    auto* ptr = static_cast<char*>(view.alloc());
    std::memset(ptr, 0xFF, 128);
    view.free(ptr);

    // calloc should zero it
    auto* clean = static_cast<char*>(view.calloc());
    REQUIRE(clean != nullptr);
    for (size_t i = 0; i < 128; ++i)
        REQUIRE(clean[i] == 0);
}

TEST_CASE("pool_view: reset restores all blocks", "[pool_view]")
{
    auto buf = make_region(64, 10);
    void* base = block_aligned_base(buf, 64);
    AL::pool_view view;
    view.init_from_region(base, 64, 10);

    for (size_t i = 0; i < 10; ++i)
        view.alloc();

    REQUIRE(view.free_count() == 0);
    view.reset();
    REQUIRE(view.free_count() == 10);

    // can allocate all again
    std::set<void*> ptrs;
    for (size_t i = 0; i < 10; ++i)
    {
        void* ptr = view.alloc();
        REQUIRE(ptr != nullptr);
        ptrs.insert(ptr);
    }
    REQUIRE(ptrs.size() == 10);
}

TEST_CASE("pool_view: free_batch", "[pool_view]")
{
    auto buf = make_region(64, 10);
    void* base = block_aligned_base(buf, 64);
    AL::pool_view view;
    view.init_from_region(base, 64, 10);

    std::vector<void*> ptrs;
    for (size_t i = 0; i < 5; ++i)
        ptrs.push_back(view.alloc());

    REQUIRE(view.free_count() == 5);
    view.free_batch(ptrs);
    REQUIRE(view.free_count() == 10);
}

TEST_CASE("pool_view: owns check", "[pool_view]")
{
    auto buf = make_region(64, 10);
    void* base = block_aligned_base(buf, 64);
    AL::pool_view view;
    view.init_from_region(base, 64, 10);

    void* ptr = view.alloc();
    REQUIRE(view.owns(ptr));
    REQUIRE_FALSE(view.owns(nullptr));

    // pointer inside a block but not block-aligned
    auto* misaligned = static_cast<std::byte*>(ptr) + 1;
    REQUIRE_FALSE(view.owns(misaligned));

    // stack pointer
    int stack_var = 0;
    REQUIRE_FALSE(view.owns(&stack_var));
}

TEST_CASE("pool_view: more than 64 blocks (multi-word bitmap)", "[pool_view]")
{
    const size_t count = 200;
    auto buf = make_region(16, count);
    void* base = block_aligned_base(buf, 16);
    AL::pool_view view;
    view.init_from_region(base, 16, count);

    REQUIRE(view.block_count() == count);
    REQUIRE(view.free_count() == count);

    std::set<void*> ptrs;
    for (size_t i = 0; i < count; ++i)
    {
        void* ptr = view.alloc();
        REQUIRE(ptr != nullptr);
        ptrs.insert(ptr);
    }
    REQUIRE(ptrs.size() == count);
    REQUIRE(view.free_count() == 0);
    REQUIRE(view.alloc() == nullptr);

    // free all and verify
    for (void* p : ptrs)
        view.free(p);
    REQUIRE(view.free_count() == count);
}

TEST_CASE("pool_view: block count not multiple of 64", "[pool_view]")
{
    // 65 blocks — last bitmap word has only 1 valid bit
    const size_t count = 65;
    auto buf = make_region(32, count);
    void* base = block_aligned_base(buf, 32);
    AL::pool_view view;
    view.init_from_region(base, 32, count);

    std::vector<void*> ptrs;
    for (size_t i = 0; i < count; ++i)
    {
        void* ptr = view.alloc();
        REQUIRE(ptr != nullptr);
        ptrs.push_back(ptr);
    }
    REQUIRE(view.alloc() == nullptr);
    REQUIRE(view.free_count() == 0);
}

TEST_CASE("pool_view: memory writes don't corrupt bitmap", "[pool_view]")
{
    auto buf = make_region(64, 10);
    void* base = block_aligned_base(buf, 64);
    AL::pool_view view;
    view.init_from_region(base, 64, 10);

    // alloc and write full blocks
    std::vector<void*> ptrs;
    for (size_t i = 0; i < 10; ++i)
    {
        auto* ptr = static_cast<char*>(view.alloc());
        std::memset(ptr, static_cast<int>(i + 1), 64);
        ptrs.push_back(ptr);
    }

    // verify data
    for (size_t i = 0; i < 10; ++i)
    {
        auto* ptr = static_cast<char*>(ptrs[i]);
        for (size_t j = 0; j < 64; ++j)
            REQUIRE(ptr[j] == static_cast<char>(i + 1));
    }

    // free should work (bitmap not corrupted)
    for (void* p : ptrs)
        view.free(p);
    REQUIRE(view.free_count() == 10);
}

TEST_CASE("pool_view: free nullptr is safe", "[pool_view]")
{
    auto buf = make_region(64, 10);
    void* base = block_aligned_base(buf, 64);
    AL::pool_view view;
    view.init_from_region(base, 64, 10);

    view.free(nullptr); // must not crash
    REQUIRE(view.free_count() == 10);
}
