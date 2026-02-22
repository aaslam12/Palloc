#include "slab.h"
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <set>
#include <vector>

TEST_CASE("Slab: Default construction", "[slab][basic]")
{
    AL::slab s;

    REQUIRE(s.get_pool_count() == 10);
    REQUIRE(s.get_total_capacity() > 0);
    REQUIRE(s.get_total_free() > 0);
    REQUIRE(s.get_total_free() <= s.get_total_capacity());
}

TEST_CASE("Slab: Construction with scale", "[slab][basic]")
{
    SECTION("Scale 1.0")
    {
        AL::slab s(1.0);
        REQUIRE(s.get_pool_count() == 10);
        REQUIRE(s.get_total_capacity() > 0);
    }

    SECTION("Scale 0.5 has less or equal capacity than scale 1.0")
    {
        AL::slab s_half(0.5);
        AL::slab s_full(1.0);
        REQUIRE(s_half.get_total_capacity() <= s_full.get_total_capacity());
    }

    SECTION("Scale 2.0 has more or equal capacity than scale 1.0")
    {
        AL::slab s_double(2.0);
        AL::slab s_full(1.0);
        REQUIRE(s_double.get_total_capacity() >= s_full.get_total_capacity());
    }

    SECTION("Very small scale still creates usable pools")
    {
        AL::slab s(0.001);
        REQUIRE(s.get_pool_count() == 10);
        REQUIRE(s.get_total_capacity() > 0);

        // Each pool should have at least 1 block
        void* ptr = s.alloc(8);
        REQUIRE(ptr != nullptr);
    }

    SECTION("Large scale")
    {
        AL::slab s(10.0);
        REQUIRE(s.get_pool_count() == 10);
        REQUIRE(s.get_total_capacity() > 0);
    }
}

TEST_CASE("Slab: Basic allocations", "[slab][alloc]")
{
    AL::slab s;

    SECTION("Small allocation")
    {
        void* ptr = s.alloc(8);
        REQUIRE(ptr != nullptr);
    }

    SECTION("Medium allocation")
    {
        void* ptr = s.alloc(128);
        REQUIRE(ptr != nullptr);
    }

    SECTION("Large allocation within range")
    {
        void* ptr = s.alloc(4096);
        REQUIRE(ptr != nullptr);
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

        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);
        REQUIRE(p3 != nullptr);
        REQUIRE(p1 != p2);
        REQUIRE(p2 != p3);
        REQUIRE(p1 != p3);
    }
}

TEST_CASE("Slab: Size class routing", "[slab][alloc]")
{
    AL::slab s;

    SECTION("Exact size class boundaries")
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
        void* p1 = s.alloc(1);  // -> 8
        void* p9 = s.alloc(9);  // -> 16
        void* p17 = s.alloc(17); // -> 32
        void* p33 = s.alloc(33); // -> 64

        REQUIRE(p1 != nullptr);
        REQUIRE(p9 != nullptr);
        REQUIRE(p17 != nullptr);
        REQUIRE(p33 != nullptr);
    }

    SECTION("Size just below boundary")
    {
        void* p7 = s.alloc(7);    // -> 8
        void* p15 = s.alloc(15);  // -> 16
        void* p31 = s.alloc(31);  // -> 32
        void* p63 = s.alloc(63);  // -> 64

        REQUIRE(p7 != nullptr);
        REQUIRE(p15 != nullptr);
        REQUIRE(p31 != nullptr);
        REQUIRE(p63 != nullptr);
    }

    SECTION("Size just above boundary")
    {
        void* p9 = s.alloc(9);     // -> 16
        void* p17 = s.alloc(17);   // -> 32
        void* p33 = s.alloc(33);   // -> 64
        void* p65 = s.alloc(65);   // -> 128

        REQUIRE(p9 != nullptr);
        REQUIRE(p17 != nullptr);
        REQUIRE(p33 != nullptr);
        REQUIRE(p65 != nullptr);
    }
}

TEST_CASE("Slab: Zero-size allocation", "[slab][alloc][edge]")
{
    AL::slab s;
    REQUIRE(s.alloc(0) == nullptr);
}

TEST_CASE("Slab: Pool exhaustion", "[slab][alloc][edge]")
{
    AL::slab s(0.01);

    SECTION("Exhaust single size class")
    {
        std::vector<void*> ptrs;
        while (true)
        {
            void* ptr = s.alloc(8);
            if (ptr == nullptr)
                break;
            ptrs.push_back(ptr);
        }

        REQUIRE(ptrs.size() > 0);
        REQUIRE(s.alloc(8) == nullptr);
    }

    SECTION("Exhausting one pool doesn't affect others")
    {
        // Exhaust 8-byte pool
        while (s.alloc(8) != nullptr)
        {
        }

        // Other pools still work
        REQUIRE(s.alloc(16) != nullptr);
        REQUIRE(s.alloc(32) != nullptr);
        REQUIRE(s.alloc(64) != nullptr);
    }
}

TEST_CASE("Slab: Calloc zeros memory", "[slab][calloc]")
{
    AL::slab s;

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

    SECTION("Multiple calloc allocations")
    {
        char* p1 = static_cast<char*>(s.calloc(32));
        char* p2 = static_cast<char*>(s.calloc(64));
        char* p3 = static_cast<char*>(s.calloc(128));

        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);
        REQUIRE(p3 != nullptr);

        for (size_t i = 0; i < 32; ++i)
            REQUIRE(p1[i] == 0);
        for (size_t i = 0; i < 64; ++i)
            REQUIRE(p2[i] == 0);
        for (size_t i = 0; i < 128; ++i)
            REQUIRE(p3[i] == 0);
    }

    SECTION("Calloc after dirty memory (alloc, write, free, calloc)")
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
}

TEST_CASE("Slab: Basic free", "[slab][free]")
{
    AL::slab s;

    SECTION("Free single allocation increases free space")
    {
        // All size classes use TLC; free goes to cache first.
        // Reset flushes caches back so pool-level accounting reflects the free.
        void* ptr = s.alloc(512);
        REQUIRE(ptr != nullptr);

        s.free(ptr, 512);
        s.reset();
        size_t free_after = s.get_total_free();

        REQUIRE(free_after == s.get_total_capacity());
    }

    SECTION("Free nullptr is safe")
    {
        size_t free_before = s.get_total_free();
        s.free(nullptr, 64);
        size_t free_after = s.get_total_free();

        REQUIRE(free_after == free_before);
    }

    SECTION("Free with zero size is safe")
    {
        void* ptr = s.alloc(64);
        size_t free_before = s.get_total_free();
        s.free(ptr, 0);
        REQUIRE(s.get_total_free() == free_before);
        // Clean up properly
        s.free(ptr, 64);
    }

    SECTION("Free with over-sized size is safe")
    {
        void* ptr = s.alloc(64);
        size_t free_before = s.get_total_free();
        s.free(ptr, 999999);
        REQUIRE(s.get_total_free() == free_before);
        s.free(ptr, 64);
    }

    SECTION("Freed block can be reallocated")
    {
        void* p1 = s.alloc(64);
        REQUIRE(p1 != nullptr);
        s.free(p1, 64);

        void* p2 = s.alloc(64);
        REQUIRE(p2 != nullptr);
    }

    SECTION("Free from different pools")
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

TEST_CASE("Slab: Reset functionality", "[slab][reset]")
{
    AL::slab s;

    SECTION("Reset empty slab preserves capacity")
    {
        size_t cap = s.get_total_capacity();
        size_t free_space = s.get_total_free();
        s.reset();

        REQUIRE(s.get_total_capacity() == cap);
        REQUIRE(s.get_total_free() == free_space);
    }

    SECTION("Reset restores free space after allocations")
    {
        size_t initial_free = s.get_total_free();

        s.alloc(32);
        s.alloc(64);
        s.alloc(128);

        REQUIRE(s.get_total_free() < initial_free);

        s.reset();

        REQUIRE(s.get_total_free() == initial_free);
    }

    SECTION("Can allocate after reset")
    {
        s.alloc(64);
        s.reset();

        void* ptr = s.alloc(64);
        REQUIRE(ptr != nullptr);
    }

    SECTION("Multiple reset cycles")
    {
        size_t initial_free = s.get_total_free();

        for (int cycle = 0; cycle < 10; ++cycle)
        {
            for (int i = 0; i < 10; ++i)
                s.alloc(64);

            s.reset();
            REQUIRE(s.get_total_free() == initial_free);
        }
    }

    SECTION("Reset with multiple pools used")
    {
        size_t initial_free = s.get_total_free();

        s.alloc(32);
        s.alloc(64);
        s.alloc(128);
        s.alloc(256);

        s.reset();

        REQUIRE(s.get_total_free() == initial_free);

        REQUIRE(s.alloc(32) != nullptr);
        REQUIRE(s.alloc(64) != nullptr);
        REQUIRE(s.alloc(128) != nullptr);
        REQUIRE(s.alloc(256) != nullptr);
    }
}

TEST_CASE("Slab: Memory integrity", "[slab][integrity]")
{
    AL::slab s;

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
        int* arr3 = static_cast<int*>(s.alloc(sizeof(int) * 10));

        REQUIRE(arr1 != nullptr);
        REQUIRE(arr2 != nullptr);
        REQUIRE(arr3 != nullptr);

        for (int i = 0; i < 10; ++i)
        {
            arr1[i] = i;
            arr2[i] = i + 100;
            arr3[i] = i + 200;
        }

        for (int i = 0; i < 10; ++i)
        {
            REQUIRE(arr1[i] == i);
            REQUIRE(arr2[i] == i + 100);
            REQUIRE(arr3[i] == i + 200);
        }
    }

    SECTION("Data persists across other allocations")
    {
        int* data = static_cast<int*>(s.alloc(sizeof(int)));
        *data = 12345;

        s.alloc(64);
        s.alloc(128);
        s.alloc(256);

        REQUIRE(*data == 12345);
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

TEST_CASE("Slab: Free space accounting", "[slab][stats]")
{
    AL::slab s;

    SECTION("Alloc decreases free space")
    {
        size_t before = s.get_total_free();
        s.alloc(64);
        REQUIRE(s.get_total_free() < before);
    }

    SECTION("Free increases free space")
    {
        // All size classes use TLC; free goes to cache, not pool.
        // Reset flushes caches back so pool-level accounting reflects the free.
        void* ptr = s.alloc(512);
        s.free(ptr, 512);
        s.reset();
        REQUIRE(s.get_total_free() == s.get_total_capacity());
    }

    SECTION("Pool-specific free space decreases on alloc")
    {
        // TLC batch-refills on first miss, so a single alloc drains
        // a batch from the pool. Verify the pool decreased.
        size_t before = s.get_pool_free_space(6);
        s.alloc(512);
        REQUIRE(s.get_pool_free_space(6) < before);
    }

    SECTION("Free space is zero after full exhaustion of a pool")
    {
        AL::slab s_small(0.01);
        // Exhaust the 8-byte pool (index 0)
        while (s_small.alloc(8) != nullptr)
        {
        }
        REQUIRE(s_small.get_pool_free_space(0) == 0);
    }
}

TEST_CASE("Slab: TLC cached class alloc returns valid memory", "[slab][tlc]")
{
    AL::slab s;

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
    AL::slab s;

    for (int i = 0; i < 200; ++i)
    {
        char* ptr = static_cast<char*>(s.alloc(32));
        REQUIRE(ptr != nullptr);
        std::memset(ptr, static_cast<int>(i & 0xFF), 32);
        REQUIRE(static_cast<unsigned char>(ptr[0]) == (i & 0xFF));
        s.free(ptr, 32);
    }
}

TEST_CASE("Slab: TLC alloc writes don't corrupt across allocations", "[slab][tlc][integrity]")
{
    AL::slab s;

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

TEST_CASE("Slab: TLC batch refill serves allocations from pool", "[slab][tlc]")
{
    AL::slab s;

    const size_t alloc_count = 200;
    std::set<void*> ptrs;

    for (size_t i = 0; i < alloc_count; ++i)
    {
        void* ptr = s.alloc(8);
        REQUIRE(ptr != nullptr);
        *static_cast<char*>(ptr) = static_cast<char>(i & 0xFF);
        ptrs.insert(ptr);
    }

    REQUIRE(ptrs.size() == alloc_count);

    for (void* ptr : ptrs)
        s.free(ptr, 8);
}

TEST_CASE("Slab: TLC epoch invalidation after reset", "[slab][tlc][reset]")
{
    AL::slab s;

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
    AL::slab s;
    size_t initial_free = s.get_total_free();

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

        *static_cast<char*>(p8) = 'A';
        *static_cast<char*>(p16) = 'B';
        *static_cast<char*>(p32) = 'C';
        *static_cast<char*>(p64) = 'D';

        s.reset();
        REQUIRE(s.get_total_free() == initial_free);
    }

    for (size_t size : {8, 16, 32, 64, 128, 256, 512})
    {
        void* ptr = s.alloc(size);
        REQUIRE(ptr != nullptr);
    }
}

TEST_CASE("Slab: TLC mixed cached and non-cached allocations", "[slab][tlc]")
{
    AL::slab s;

    size_t sizes[] = {8, 128, 16, 256, 32, 512, 64, 1024};

    for (int round = 0; round < 20; ++round)
    {
        for (size_t size : sizes)
        {
            void* ptr = s.alloc(size);
            REQUIRE(ptr != nullptr);
            s.free(ptr, size);
        }
    }
}

TEST_CASE("Slab: TLC exhaust cached pool completely", "[slab][tlc][edge]")
{
    AL::slab s(0.01);

    std::vector<void*> ptrs;
    while (true)
    {
        void* ptr = s.alloc(8);
        if (ptr == nullptr)
            break;
        ptrs.push_back(ptr);
    }

    REQUIRE(ptrs.size() > 0);
    REQUIRE(s.alloc(8) == nullptr);

    for (void* ptr : ptrs)
        s.free(ptr, 8);
}

TEST_CASE("Slab: TLC cache handles rapid alloc churn", "[slab][tlc]")
{
    AL::slab s;

    for (int i = 0; i < 200; ++i)
    {
        void* ptr = s.alloc(64);
        REQUIRE(ptr != nullptr);
        *static_cast<int*>(ptr) = i;
        REQUIRE(*static_cast<int*>(ptr) == i);
        s.free(ptr, 64);
    }
}

// Note: slab move constructor/assignment are declared = default but are
// implicitly deleted because std::atomic<size_t> is not copy/move-constructible.
// Move semantics would require a user-defined implementation.
// Tests are omitted since move is not currently supported.

TEST_CASE("Slab: Calloc on TLC-cached sizes zeros memory", "[slab][tlc][calloc]")
{
    AL::slab s;

    SECTION("Multiple callocs on different cached sizes")
    {
        for (size_t size : {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096})
        {
            char* ptr = static_cast<char*>(s.calloc(size));
            REQUIRE(ptr != nullptr);
            for (size_t i = 0; i < size; ++i)
                REQUIRE(ptr[i] == 0);
            s.free(ptr, size);
        }
    }
}
