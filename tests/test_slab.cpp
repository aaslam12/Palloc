#include "slab.h"
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <set>
#include <vector>

constexpr std::array<AL::size_class, 3> TINY_CONFIG = {
    {
     {.byte_size = 8, .num_blocks = 4, .batch_size = 2},
     {.byte_size = 16, .num_blocks = 4, .batch_size = 2},
     {.byte_size = 32, .num_blocks = 4, .batch_size = 2},
     }
};
using tiny_slab = AL::slab<AL::slab_config<3, TINY_CONFIG>>;

constexpr std::array<AL::size_class, 2> LARGE_CONFIG = {
    {
     {.byte_size = 64, .num_blocks = 1024, .batch_size = 64},
     {.byte_size = 128, .num_blocks = 1024, .batch_size = 64},
     }
};
using large_slab = AL::slab<AL::slab_config<2, LARGE_CONFIG>>;

constexpr std::array<AL::size_class, 1> SINGLE_CONFIG = {
    {
     {.byte_size = 8, .num_blocks = 1, .batch_size = 1},
     }
};
using single_slab = AL::slab<AL::slab_config<1, SINGLE_CONFIG>>;

// ──────────────────────────────────────────────────────────────────────────────
// Construction
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Slab: Default construction", "[slab][basic]")
{
    AL::default_slab s;
    REQUIRE(s.get_pool_count() == 10);
    REQUIRE(s.get_total_capacity() > 0);
    REQUIRE(s.get_total_free() > 0);
    REQUIRE(s.get_total_free() <= s.get_total_capacity());
}

TEST_CASE("Slab: Custom size class configs", "[slab][config]")
{
    SECTION("tiny_slab has 3 pools")
    {
        tiny_slab s;
        REQUIRE(s.get_pool_count() == 3);
        REQUIRE(s.get_total_capacity() > 0);
    }

    SECTION("large_slab has 2 pools with large capacity")
    {
        large_slab s;
        REQUIRE(s.get_pool_count() == 2);
        REQUIRE(s.get_total_capacity() > 0);
    }

    SECTION("tiny_slab has less capacity than large_slab")
    {
        tiny_slab s_tiny;
        large_slab s_large;
        REQUIRE(s_tiny.get_total_capacity() < s_large.get_total_capacity());
    }

    SECTION("single block config: exactly one allocation succeeds")
    {
        single_slab s;
        REQUIRE(s.get_pool_count() == 1);
        void* p = s.alloc(8);
        REQUIRE(p != nullptr);
        REQUIRE(s.alloc(8) == nullptr); // exhausted
    }

    SECTION("custom config rejects sizes outside its range")
    {
        tiny_slab s;
        REQUIRE(s.alloc(64) == nullptr); // 64 > max class (32)
        REQUIRE(s.alloc(0) == nullptr);
    }

    SECTION("custom config allocates all its size classes")
    {
        tiny_slab s;
        void* p8 = s.alloc(8);
        void* p16 = s.alloc(16);
        void* p32 = s.alloc(32);
        REQUIRE(p8 != nullptr);
        REQUIRE(p16 != nullptr);
        REQUIRE(p32 != nullptr);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Basic allocations
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Slab: Basic allocations", "[slab][alloc]")
{
    AL::default_slab s;

    SECTION("Small allocation")
    {
        REQUIRE(s.alloc(8) != nullptr);
    }

    SECTION("Medium allocation")
    {
        REQUIRE(s.alloc(128) != nullptr);
    }

    SECTION("Large allocation within range")
    {
        REQUIRE(s.alloc(4096) != nullptr);
    }

    SECTION("Multiple distinct allocations same size")
    {
        void* p1 = s.alloc(64);
        void* p2 = s.alloc(64);
        void* p3 = s.alloc(64);
        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);
        REQUIRE(p3 != nullptr);
        REQUIRE(p1 != p2);
        REQUIRE(p2 != p3);
        REQUIRE(p1 != p3);
    }

    SECTION("Multiple distinct allocations different sizes")
    {
        void* p1 = s.alloc(32);
        void* p2 = s.alloc(64);
        void* p3 = s.alloc(128);
        REQUIRE(p1 != p2);
        REQUIRE(p2 != p3);
        REQUIRE(p1 != p3);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Size class routing
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Slab: Size class routing", "[slab][alloc]")
{
    AL::default_slab s;

    SECTION("Exact size class boundaries all succeed")
    {
        size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
        for (size_t size : sizes)
        {
            void* ptr = s.alloc(size);
            REQUIRE(ptr != nullptr);
            s.free(ptr, size);
        }
    }

    SECTION("Non-exact sizes use next larger class")
    {
        REQUIRE(s.alloc(1) != nullptr);  // → 8
        REQUIRE(s.alloc(9) != nullptr);  // → 16
        REQUIRE(s.alloc(17) != nullptr); // → 32
        REQUIRE(s.alloc(33) != nullptr); // → 64
    }

    SECTION("Size just below boundary")
    {
        REQUIRE(s.alloc(7) != nullptr);  // → 8
        REQUIRE(s.alloc(15) != nullptr); // → 16
        REQUIRE(s.alloc(31) != nullptr); // → 32
        REQUIRE(s.alloc(63) != nullptr); // → 64
    }

    SECTION("Size just above boundary routes up")
    {
        REQUIRE(s.alloc(9) != nullptr);  // → 16
        REQUIRE(s.alloc(17) != nullptr); // → 32
        REQUIRE(s.alloc(33) != nullptr); // → 64
        REQUIRE(s.alloc(65) != nullptr); // → 128
    }

    SECTION("Size above max class returns nullptr")
    {
        REQUIRE(s.alloc(4097) == nullptr);
        REQUIRE(s.alloc(static_cast<size_t>(-1)) == nullptr);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Edge cases
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Slab: Zero-size allocation", "[slab][alloc][edge]")
{
    AL::default_slab s;
    REQUIRE(s.alloc(0) == nullptr);
}

TEST_CASE("Slab: Pool exhaustion", "[slab][alloc][edge]")
{
    tiny_slab s;

    SECTION("Exhaust single size class")
    {
        std::vector<void*> ptrs;
        while (void* ptr = s.alloc(8))
            ptrs.push_back(ptr);

        REQUIRE(!ptrs.empty());
        REQUIRE(s.alloc(8) == nullptr);
    }

    SECTION("Exhausting one pool doesn't affect others")
    {
        while (s.alloc(8) != nullptr)
        {}
        REQUIRE(s.alloc(16) != nullptr);
        REQUIRE(s.alloc(32) != nullptr);
    }

    SECTION("After reset exhausted pool is usable again")
    {
        while (s.alloc(8) != nullptr)
        {}
        REQUIRE(s.alloc(8) == nullptr);
        s.reset();
        REQUIRE(s.alloc(8) != nullptr);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Calloc
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Slab: Calloc zeros memory", "[slab][calloc]")
{
    AL::default_slab s;

    SECTION("Small calloc")
    {
        char* ptr = static_cast<char*>(s.calloc(64));
        REQUIRE(ptr != nullptr);
        for (size_t i = 0; i < 64; ++i)
            REQUIRE(ptr[i] == 0);
    }

    SECTION("Large calloc")
    {
        char* ptr = static_cast<char*>(s.calloc(1024));
        REQUIRE(ptr != nullptr);
        for (size_t i = 0; i < 1024; ++i)
            REQUIRE(ptr[i] == 0);
    }

    SECTION("Calloc after dirty memory")
    {
        char* ptr1 = static_cast<char*>(s.alloc(128));
        REQUIRE(ptr1 != nullptr);
        std::memset(ptr1, 0xFF, 128);
        s.free(ptr1, 128);

        char* ptr2 = static_cast<char*>(s.calloc(128));
        REQUIRE(ptr2 != nullptr);
        for (size_t i = 0; i < 128; ++i)
            REQUIRE(ptr2[i] == 0);
    }

    SECTION("Calloc zero size returns nullptr")
    {
        REQUIRE(s.calloc(0) == nullptr);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Free
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Slab: Basic free", "[slab][free]")
{
    AL::default_slab s;

    SECTION("Free single allocation restores capacity after reset")
    {
        void* ptr = s.alloc(512);
        REQUIRE(ptr != nullptr);
        s.free(ptr, 512);
        s.reset();
        REQUIRE(s.get_total_free() == s.get_total_capacity());
    }

    SECTION("Free nullptr is safe")
    {
        size_t before = s.get_total_free();
        s.free(nullptr, 64);
        REQUIRE(s.get_total_free() == before);
    }

    SECTION("Free with zero size is a no-op")
    {
        void* ptr = s.alloc(64);
        size_t before = s.get_total_free();
        s.free(ptr, 0);
        REQUIRE(s.get_total_free() == before);
        s.free(ptr, 64);
    }

    SECTION("Free with oversized size is a no-op")
    {
        void* ptr = s.alloc(64);
        size_t before = s.get_total_free();
        s.free(ptr, 999999);
        REQUIRE(s.get_total_free() == before);
        s.free(ptr, 64);
    }

    SECTION("Freed block can be reallocated")
    {
        void* p1 = s.alloc(64);
        REQUIRE(p1 != nullptr);
        s.free(p1, 64);
        REQUIRE(s.alloc(64) != nullptr);
    }

    SECTION("Free from multiple pools")
    {
        void* p32 = s.alloc(32);
        void* p64 = s.alloc(64);
        void* p128 = s.alloc(128);
        s.free(p32, 32);
        s.free(p64, 64);
        s.free(p128, 128);
        REQUIRE(s.alloc(32) != nullptr);
        REQUIRE(s.alloc(64) != nullptr);
        REQUIRE(s.alloc(128) != nullptr);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Reset
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Slab: Reset functionality", "[slab][reset]")
{
    AL::default_slab s;

    SECTION("Reset empty slab preserves capacity and free")
    {
        size_t cap = s.get_total_capacity();
        size_t free = s.get_total_free();
        s.reset();
        REQUIRE(s.get_total_capacity() == cap);
        REQUIRE(s.get_total_free() == free);
    }

    SECTION("Reset restores free space after allocations")
    {
        size_t initial = s.get_total_free();
        s.alloc(32);
        s.alloc(64);
        s.alloc(128);
        REQUIRE(s.get_total_free() < initial);
        s.reset();
        REQUIRE(s.get_total_free() == initial);
    }

    SECTION("Can allocate after reset")
    {
        s.alloc(64);
        s.reset();
        REQUIRE(s.alloc(64) != nullptr);
    }

    SECTION("Multiple reset cycles keep free space stable")
    {
        size_t initial = s.get_total_free();
        for (int cycle = 0; cycle < 10; ++cycle)
        {
            for (int i = 0; i < 10; ++i)
                s.alloc(64);
            s.reset();
            REQUIRE(s.get_total_free() == initial);
        }
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Memory integrity
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Slab: Memory integrity", "[slab][integrity]")
{
    AL::default_slab s;

    SECTION("Write and read structured data")
    {
        struct TestData
        {
            int x;
            double y;
            char z[32];
        };
        TestData* data = static_cast<TestData*>(s.alloc(sizeof(TestData)));
        REQUIRE(data != nullptr);
        data->x = 42;
        data->y = 3.14159;
        std::strcpy(data->z, "hello");
        REQUIRE(data->x == 42);
        REQUIRE(data->y == 3.14159);
        REQUIRE(std::strcmp(data->z, "hello") == 0);
    }

    SECTION("Multiple allocations don't interfere")
    {
        int* arr1 = static_cast<int*>(s.alloc(sizeof(int) * 10));
        int* arr2 = static_cast<int*>(s.alloc(sizeof(int) * 10));
        for (int i = 0; i < 10; ++i)
        {
            arr1[i] = i;
            arr2[i] = i + 100;
        }
        for (int i = 0; i < 10; ++i)
        {
            REQUIRE(arr1[i] == i);
            REQUIRE(arr2[i] == i + 100);
        }
    }

    SECTION("Large buffer integrity")
    {
        char* buf = static_cast<char*>(s.alloc(4096));
        REQUIRE(buf != nullptr);
        for (size_t i = 0; i < 4096; ++i)
            buf[i] = static_cast<char>(i % 256);
        for (size_t i = 0; i < 4096; ++i)
            REQUIRE(buf[i] == static_cast<char>(i % 256));
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Free space accounting
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Slab: Free space accounting", "[slab][stats]")
{
    AL::default_slab s;

    SECTION("Alloc decreases free space")
    {
        size_t before = s.get_total_free();
        s.alloc(64);
        REQUIRE(s.get_total_free() < before);
    }

    SECTION("Free + reset restores full capacity")
    {
        void* ptr = s.alloc(512);
        s.free(ptr, 512);
        s.reset();
        REQUIRE(s.get_total_free() == s.get_total_capacity());
    }

    SECTION("Pool-specific free space decreases on alloc")
    {
        size_t before = s.get_pool_free_space(6); // 512B pool
        s.alloc(512);
        REQUIRE(s.get_pool_free_space(6) < before);
    }

    SECTION("Free space is zero after exhausting a pool")
    {
        tiny_slab s_tiny;
        while (s_tiny.alloc(8) != nullptr)
        {}
        REQUIRE(s_tiny.get_pool_free_space(0) == 0);
    }

    SECTION("get_pool_block_size returns correct sizes")
    {
        AL::default_slab sd;
        REQUIRE(sd.get_pool_block_size(0) == 8);
        REQUIRE(sd.get_pool_block_size(1) == 16);
        REQUIRE(sd.get_pool_block_size(9) == 4096);
        REQUIRE(sd.get_pool_block_size(10) == 0); // out of range
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Thread-local cache (TLC)
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Slab: TLC cached class alloc returns valid memory", "[slab][tlc]")
{
    AL::default_slab s;

    SECTION("All cached size classes return writable memory")
    {
        size_t cached_sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
        for (size_t size : cached_sizes)
        {
            char* ptr = static_cast<char*>(s.alloc(size));
            REQUIRE(ptr != nullptr);
            std::memset(ptr, 0xAB, size);
            REQUIRE(static_cast<unsigned char>(ptr[0]) == 0xAB);
            REQUIRE(static_cast<unsigned char>(ptr[size - 1]) == 0xAB);
            s.free(ptr, size);
        }
    }

    SECTION("Sub-boundary sizes routed to cached class")
    {
        void* p1 = s.alloc(1);
        void* p5 = s.alloc(5);
        void* p9 = s.alloc(9);
        void* p17 = s.alloc(17);
        void* p33 = s.alloc(33);
        REQUIRE(p1 != nullptr);
        REQUIRE(p5 != nullptr);
        REQUIRE(p9 != nullptr);
        REQUIRE(p17 != nullptr);
        REQUIRE(p33 != nullptr);
        s.free(p1, 1);
        s.free(p5, 5);
        s.free(p9, 9);
        s.free(p17, 17);
        s.free(p33, 33);
    }
}

TEST_CASE("Slab: TLC multiple allocs return valid memory", "[slab][tlc]")
{
    AL::default_slab s;
    for (int i = 0; i < 200; ++i)
    {
        char* ptr = static_cast<char*>(s.alloc(32));
        REQUIRE(ptr != nullptr);
        std::memset(ptr, i & 0xFF, 32);
        REQUIRE(static_cast<unsigned char>(ptr[0]) == (i & 0xFF));
        s.free(ptr, 32);
    }
}

TEST_CASE("Slab: TLC alloc writes don't corrupt across allocations", "[slab][tlc][integrity]")
{
    AL::default_slab s;
    constexpr size_t count = 50;
    std::vector<char*> ptrs;
    ptrs.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        char* ptr = static_cast<char*>(s.alloc(64));
        REQUIRE(ptr != nullptr);
        std::memset(ptr, static_cast<int>(i & 0xFF), 64);
        ptrs.push_back(ptr);
    }
    for (size_t i = 0; i < count; ++i)
    {
        REQUIRE(static_cast<unsigned char>(ptrs[i][0]) == (i & 0xFF));
        REQUIRE(static_cast<unsigned char>(ptrs[i][63]) == (i & 0xFF));
    }
    for (size_t i = 0; i < count; ++i)
        s.free(ptrs[i], 64);
}

TEST_CASE("Slab: TLC batch refill returns unique pointers", "[slab][tlc]")
{
    AL::default_slab s;
    const size_t count = 200;
    std::set<void*> ptrs;
    for (size_t i = 0; i < count; ++i)
    {
        void* ptr = s.alloc(8);
        REQUIRE(ptr != nullptr);
        ptrs.insert(ptr);
    }
    REQUIRE(ptrs.size() == count);
    for (void* ptr : ptrs)
        s.free(ptr, 8);
}

TEST_CASE("Slab: TLC epoch invalidation after reset", "[slab][tlc][reset]")
{
    AL::default_slab s;
    void* ptr1 = s.alloc(16);
    REQUIRE(ptr1 != nullptr);
    s.reset();
    void* ptr2 = s.alloc(16);
    REQUIRE(ptr2 != nullptr);
    std::memset(ptr2, 0xCD, 16);
    REQUIRE(static_cast<unsigned char>(static_cast<char*>(ptr2)[0]) == 0xCD);
}

TEST_CASE("Slab: TLC multiple sequential resets", "[slab][tlc][reset]")
{
    AL::default_slab s;
    size_t initial = s.get_total_free();
    for (int cycle = 0; cycle < 10; ++cycle)
    {
        void* p8 = s.alloc(8);
        void* p16 = s.alloc(16);
        void* p32 = s.alloc(32);
        void* p64 = s.alloc(64);
        REQUIRE(p8 != nullptr);
        REQUIRE(p16 != nullptr);
        REQUIRE(p32 != nullptr);
        REQUIRE(p64 != nullptr);
        s.reset();
        REQUIRE(s.get_total_free() == initial);
    }
}

TEST_CASE("Slab: TLC exhaust cached pool completely", "[slab][tlc][edge]")
{
    tiny_slab s;
    std::vector<void*> ptrs;
    while (void* ptr = s.alloc(8))
        ptrs.push_back(ptr);
    REQUIRE(!ptrs.empty());
    REQUIRE(s.alloc(8) == nullptr);
    for (void* ptr : ptrs)
        s.free(ptr, 8);
}

TEST_CASE("Slab: TLC cache handles rapid alloc/free churn", "[slab][tlc]")
{
    AL::default_slab s;
    for (int i = 0; i < 200; ++i)
    {
        void* ptr = s.alloc(64);
        REQUIRE(ptr != nullptr);
        *static_cast<int*>(ptr) = i;
        REQUIRE(*static_cast<int*>(ptr) == i);
        s.free(ptr, 64);
    }
}

TEST_CASE("Slab: TLC mixed cached and non-cached allocations", "[slab][tlc]")
{
    AL::default_slab s;
    size_t sizes[] = {8, 128, 16, 256, 32, 512, 64, 1024};
    for (int round = 0; round < 20; ++round)
        for (size_t size : sizes)
        {
            void* ptr = s.alloc(size);
            REQUIRE(ptr != nullptr);
            s.free(ptr, size);
        }
}

TEST_CASE("Slab: Calloc on TLC-cached sizes zeros memory", "[slab][tlc][calloc]")
{
    AL::default_slab s;
    for (size_t size : {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096})
    {
        char* ptr = static_cast<char*>(s.calloc(size));
        REQUIRE(ptr != nullptr);
        for (size_t i = 0; i < size; ++i)
            REQUIRE(ptr[i] == 0);
        s.free(ptr, size);
    }
}

TEST_CASE("Slab: owns() correctly identifies belonging pointers", "[slab][owns]")
{
    AL::default_slab s;
    void* ptr = s.alloc(64);
    REQUIRE(ptr != nullptr);
    REQUIRE(s.owns(ptr));
}

TEST_CASE("Slab: Two slabs with same config don't share pools", "[slab][isolation]")
{
    AL::default_slab s1;
    AL::default_slab s2;
    void* p1 = s1.alloc(64);
    void* p2 = s2.alloc(64);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    REQUIRE(p1 != p2);
    REQUIRE(s1.owns(p1));
    REQUIRE(!s1.owns(p2));
    REQUIRE(s2.owns(p2));
    REQUIRE(!s2.owns(p1));
}

TEST_CASE("Slab: Two slabs with different configs are independent", "[slab][isolation]")
{
    AL::default_slab sd;
    large_slab sl;
    void* pd = sd.alloc(64);
    void* pl = sl.alloc(64);
    REQUIRE(pd != nullptr);
    REQUIRE(pl != nullptr);
    REQUIRE(!sl.owns(pd));
    REQUIRE(!sd.owns(pl));
}

// ──────────────────────────────────────────────────────────────────────────────
// Sparse (non-dense) configs
// ──────────────────────────────────────────────────────────────────────────────

constexpr std::array<AL::size_class, 2> SPARSE_CONFIG = {
    {
     {.byte_size = 8, .num_blocks = 64, .batch_size = 8},
     {.byte_size = 64, .num_blocks = 64, .batch_size = 8},
     }
};
using sparse_slab = AL::slab<AL::slab_config<2, SPARSE_CONFIG, 2>>;

TEST_CASE("Slab: Sparse config — exact size allocation", "[slab][sparse]")
{
    sparse_slab s;
    REQUIRE(s.get_pool_count() == 2);

    void* p8 = s.alloc(8);
    void* p64 = s.alloc(64);
    REQUIRE(p8 != nullptr);
    REQUIRE(p64 != nullptr);
    REQUIRE(p8 != p64);

    s.free(p8, 8);
    s.free(p64, 64);
}

TEST_CASE("Slab: Sparse config — gap sizes round up", "[slab][sparse]")
{
    sparse_slab s;

    // sizes 9-64 should all go to the 64B pool (pool index 1)
    void* p16 = s.alloc(16);
    void* p32 = s.alloc(32);
    void* p48 = s.alloc(48);
    REQUIRE(p16 != nullptr);
    REQUIRE(p32 != nullptr);
    REQUIRE(p48 != nullptr);

    // all are from the 64B pool — verify they're 64B-aligned
    REQUIRE((reinterpret_cast<uintptr_t>(p16) % 64) == 0);
    REQUIRE((reinterpret_cast<uintptr_t>(p32) % 64) == 0);
    REQUIRE((reinterpret_cast<uintptr_t>(p48) % 64) == 0);

    s.free(p16, 16);
    s.free(p32, 32);
    s.free(p48, 48);
}

TEST_CASE("Slab: Sparse config — exhaustion and size_to_index", "[slab][sparse]")
{
    using config = AL::slab_config<2, SPARSE_CONFIG, 2>;

    // verify LUT mapping
    REQUIRE(config::INDEX_LUT[0] == 0); // 8B → pool 0
    REQUIRE(config::INDEX_LUT[1] == 1); // 16B → pool 1 (64B)
    REQUIRE(config::INDEX_LUT[2] == 1); // 32B → pool 1 (64B)
    REQUIRE(config::INDEX_LUT[3] == 1); // 64B → pool 1 (64B)

    // size_to_index
    REQUIRE(sparse_slab::size_to_index(8) == 0);
    REQUIRE(sparse_slab::size_to_index(16) == 1);
    REQUIRE(sparse_slab::size_to_index(32) == 1);
    REQUIRE(sparse_slab::size_to_index(64) == 1);
    REQUIRE(sparse_slab::size_to_index(128) == static_cast<size_t>(-1));
    REQUIRE(sparse_slab::size_to_index(0) == static_cast<size_t>(-1));
}

constexpr std::array<AL::size_class, 3> WIDE_SPARSE_CONFIG = {
    {
     {.byte_size = 8, .num_blocks = 32, .batch_size = 4},
     {.byte_size = 128, .num_blocks = 32, .batch_size = 4},
     {.byte_size = 4096, .num_blocks = 8, .batch_size = 2},
     }
};
using wide_sparse_slab = AL::slab<AL::slab_config<3, WIDE_SPARSE_CONFIG, 3>>;

TEST_CASE("Slab: Wide sparse config {8, 128, 4096}", "[slab][sparse]")
{
    wide_sparse_slab s;
    REQUIRE(s.get_pool_count() == 3);

    void* p8 = s.alloc(8);
    void* p128 = s.alloc(128);
    void* p4096 = s.alloc(4096);
    REQUIRE(p8 != nullptr);
    REQUIRE(p128 != nullptr);
    REQUIRE(p4096 != nullptr);

    // intermediate sizes round up
    void* p16 = s.alloc(16);   // → 128B pool
    void* p256 = s.alloc(256); // → 4096B pool
    REQUIRE(p16 != nullptr);
    REQUIRE(p256 != nullptr);

    s.free(p8, 8);
    s.free(p128, 128);
    s.free(p4096, 4096);
    s.free(p16, 16);
    s.free(p256, 256);
}
