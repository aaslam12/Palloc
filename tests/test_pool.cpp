#include "pool.h"
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <set>
#include <unistd.h>
#include <vector>

const size_t PAGE_SIZE = getpagesize();

TEST_CASE("Pool: Basic construction", "[pool][basic]")
{
    SECTION("Single block pool")
    {
        AL::pool<> p(64, 1);
        REQUIRE(p.get_capacity() >= 64);
        REQUIRE(p.get_free_space() >= 64);
    }

    SECTION("Multiple blocks pool")
    {
        AL::pool<> p(64, 100);
        REQUIRE(p.get_capacity() >= 64 * 100);
        REQUIRE(p.get_free_space() == 64 * 100);
    }

    SECTION("Small block size (rounds up to pointer size)")
    {
        AL::pool<> p(1, 10);
        REQUIRE(p.get_capacity() >= sizeof(void*) * 10);
    }

    SECTION("Large block size (power of 2 rounding)")
    {
        AL::pool<> p(100, 10);
        // Should round up to next power of 2
        REQUIRE(p.get_capacity() >= 128 * 10);
    }
}

TEST_CASE("Pool: Basic allocations", "[pool][alloc]")
{
    AL::pool<> p(64, 10);

    SECTION("Single allocation")
    {
        void* ptr = p.alloc();
        REQUIRE(ptr != nullptr);
        REQUIRE(p.get_free_space() < p.get_capacity());
    }

    SECTION("Multiple allocations return distinct pointers")
    {
        void* p1 = p.alloc();
        void* p2 = p.alloc();
        void* p3 = p.alloc();

        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);
        REQUIRE(p3 != nullptr);

        REQUIRE(p1 != p2);
        REQUIRE(p2 != p3);
        REQUIRE(p1 != p3);
    }

    SECTION("Allocate all blocks")
    {
        std::vector<void*> ptrs;

        for (int i = 0; i < 10; ++i)
        {
            void* ptr = p.alloc();
            REQUIRE(ptr != nullptr);
            ptrs.push_back(ptr);
        }

        // All blocks allocated
        void* ptr = p.alloc();
        REQUIRE(ptr == nullptr);
    }

    SECTION("Allocations are properly aligned")
    {
        void* p1 = p.alloc();
        void* p2 = p.alloc();

        // Check alignment: either power of 2 aligned or pointer-size aligned
        bool p1_aligned = (reinterpret_cast<uintptr_t>(p1) % 64 == 0) || (reinterpret_cast<uintptr_t>(p1) % sizeof(void*) == 0);
        bool p2_aligned = (reinterpret_cast<uintptr_t>(p2) % 64 == 0) || (reinterpret_cast<uintptr_t>(p2) % sizeof(void*) == 0);

        REQUIRE(p1_aligned);
        REQUIRE(p2_aligned);
    }
}

TEST_CASE("Pool: Allocation exhaustion", "[pool][alloc][edge]")
{
    AL::pool<> p(64, 5);

    SECTION("Exactly fill pool")
    {
        std::vector<void*> ptrs;

        for (int i = 0; i < 5; ++i)
        {
            void* ptr = p.alloc();
            REQUIRE(ptr != nullptr);
            ptrs.push_back(ptr);
        }

        // Pool should be full
        void* ptr = p.alloc();
        REQUIRE(ptr == nullptr);

        // Free space should be 0
        REQUIRE(p.get_free_space() == 0);
    }

    SECTION("Allocation after exhaustion returns nullptr")
    {
        // Fill pool
        for (int i = 0; i < 5; ++i)
        {
            p.alloc();
        }

        // Try multiple times to allocate from full pool
        for (int i = 0; i < 10; ++i)
        {
            void* ptr = p.alloc();
            REQUIRE(ptr == nullptr);
        }
    }
}

TEST_CASE("Pool: Basic free", "[pool][free]")
{
    AL::pool<> p(64, 10);

    SECTION("Free single allocation")
    {
        void* ptr = p.alloc();
        REQUIRE(ptr != nullptr);

        size_t free_before = p.get_free_space();
        p.free(ptr);
        size_t free_after = p.get_free_space();

        REQUIRE(free_after > free_before);
    }

    SECTION("Free nullptr is safe")
    {
        size_t free_before = p.get_free_space();
        p.free(nullptr);
        size_t free_after = p.get_free_space();

        REQUIRE(free_after == free_before);
    }

    SECTION("Freed block can be reallocated")
    {
        void* p1 = p.alloc();
        REQUIRE(p1 != nullptr);

        p.free(p1);

        void* p2 = p.alloc();
        REQUIRE(p2 != nullptr);
        REQUIRE(p2 == p1); // Should get same block back
    }

    SECTION("Free multiple allocations")
    {
        void* p1 = p.alloc();
        void* p2 = p.alloc();
        void* p3 = p.alloc();

        p.free(p1);
        p.free(p2);
        p.free(p3);

        // Should be able to allocate again
        void* p4 = p.alloc();
        void* p5 = p.alloc();
        void* p6 = p.alloc();

        REQUIRE(p4 != nullptr);
        REQUIRE(p5 != nullptr);
        REQUIRE(p6 != nullptr);
    }
}

TEST_CASE("Pool: Calloc zeros memory", "[pool][calloc]")
{
    AL::pool<> p(128, 10);

    SECTION("Calloc single block")
    {
        char* ptr = static_cast<char*>(p.calloc());
        REQUIRE(ptr != nullptr);

        for (size_t i = 0; i < 128; ++i)
        {
            REQUIRE(ptr[i] == 0);
        }
    }

    SECTION("Calloc after dirty memory")
    {
        char* ptr1 = static_cast<char*>(p.alloc());
        std::memset(ptr1, 0xFF, 128);
        p.free(ptr1);

        char* ptr2 = static_cast<char*>(p.calloc());
        REQUIRE(ptr2 != nullptr);

        for (size_t i = 0; i < 128; ++i)
        {
            REQUIRE(ptr2[i] == 0);
        }
    }

    SECTION("Multiple calloc allocations")
    {
        char* p1 = static_cast<char*>(p.calloc());
        char* p2 = static_cast<char*>(p.calloc());

        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);

        for (size_t i = 0; i < 128; ++i)
        {
            REQUIRE(p1[i] == 0);
            REQUIRE(p2[i] == 0);
        }
    }
}

TEST_CASE("Pool: Reset functionality", "[pool][reset]")
{
    AL::pool<> p(64, 10);

    SECTION("Reset empty pool")
    {
        size_t cap_before = p.get_capacity();
        size_t free_before = p.get_free_space();

        p.reset();

        REQUIRE(p.get_capacity() == cap_before);
        REQUIRE(p.get_free_space() == free_before);
    }

    SECTION("Reset partially used pool")
    {
        void* p1 = p.alloc();
        void* p2 = p.alloc();
        void* p3 = p.alloc();

        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);
        REQUIRE(p3 != nullptr);

        p.reset();

        REQUIRE(p.get_free_space() == 64 * 10); // block_size * block_count

        // Should be able to allocate again
        void* p4 = p.alloc();
        REQUIRE(p4 != nullptr);
    }

    SECTION("Reset full pool")
    {
        // Fill pool
        std::vector<void*> ptrs;
        for (int i = 0; i < 10; ++i)
        {
            ptrs.push_back(p.alloc());
        }

        REQUIRE(p.get_free_space() == 0);

        p.reset();

        REQUIRE(p.get_free_space() == 64 * 10); // block_size * block_count

        // All blocks should be available
        for (int i = 0; i < 10; ++i)
        {
            void* ptr = p.alloc();
            REQUIRE(ptr != nullptr);
        }
    }

    SECTION("Multiple reset cycles")
    {
        for (int cycle = 0; cycle < 100; ++cycle)
        {
            // Allocate some blocks
            for (int i = 0; i < 5; ++i)
            {
                p.alloc();
            }

            p.reset();
            REQUIRE(p.get_free_space() == 64 * 10); // block_size * block_count
        }
    }
}

TEST_CASE("Pool: Memory integrity", "[pool][integrity]")
{
    AL::pool<> p(128, 10);

    SECTION("Write and read data")
    {
        struct TestData
        {
            int a;
            double b;
            char c[64];
        };

        TestData* data = static_cast<TestData*>(p.alloc());
        REQUIRE(data != nullptr);

        data->a = 42;
        data->b = 3.14159;
        std::strcpy(data->c, "test string");

        REQUIRE(data->a == 42);
        REQUIRE(data->b == 3.14159);
        REQUIRE(std::strcmp(data->c, "test string") == 0);
    }

    SECTION("Multiple allocations don't interfere")
    {
        int* arr1 = static_cast<int*>(p.alloc());
        int* arr2 = static_cast<int*>(p.alloc());
        int* arr3 = static_cast<int*>(p.alloc());

        // Fill with patterns
        for (size_t i = 0; i < 32; ++i)
        {
            arr1[i] = i;
            arr2[i] = i + 1000;
            arr3[i] = i + 2000;
        }

        // Verify no corruption
        for (size_t i = 0; i < 32; ++i)
        {
            REQUIRE(arr1[i] == static_cast<int>(i));
            REQUIRE(arr2[i] == static_cast<int>(i + 1000));
            REQUIRE(arr3[i] == static_cast<int>(i + 2000));
        }
    }

    SECTION("Data persists until freed")
    {
        int* data = static_cast<int*>(p.alloc());
        *data = 12345;

        // Allocate more blocks
        p.alloc();
        p.alloc();

        // Original data should still be intact
        REQUIRE(*data == 12345);

        p.free(data);
    }
}

TEST_CASE("Pool: Deferred initialization via init()", "[pool][init]")
{
    SECTION("Default construct then init")
    {
        AL::pool<> p;
        p.init(64, 10);

        REQUIRE(p.get_block_size() == 64);
        REQUIRE(p.get_block_count() == 10);
        REQUIRE(p.get_free_space() == 64 * 10);

        void* ptr = p.alloc();
        REQUIRE(ptr != nullptr);
        p.free(ptr);
    }

    SECTION("Init with small block size rounds up")
    {
        AL::pool<> p;
        p.init(1, 10);

        REQUIRE(p.get_block_size() == sizeof(void*));
    }

    SECTION("Init with non-power-of-2 rounds up")
    {
        AL::pool<> p;
        p.init(100, 5);

        REQUIRE(p.get_block_size() == 128);
        REQUIRE(p.get_block_count() == 5);
    }
}

TEST_CASE("Pool: Move constructor", "[pool][move]")
{
    SECTION("Move transfers ownership")
    {
        AL::pool<> src(64, 10);
        void* ptr = src.alloc();
        REQUIRE(ptr != nullptr);
        src.free(ptr);

        size_t src_capacity = src.get_capacity();
        size_t src_block_size = src.get_block_size();
        size_t src_block_count = src.get_block_count();
        size_t src_free = src.get_free_space();

        AL::pool<> dst(std::move(src));

        REQUIRE(dst.get_capacity() == src_capacity);
        REQUIRE(dst.get_block_size() == src_block_size);
        REQUIRE(dst.get_block_count() == src_block_count);
        REQUIRE(dst.get_free_space() == src_free);
    }

    SECTION("Moved-to pool is usable")
    {
        AL::pool<> src(128, 5);
        AL::pool<> dst(std::move(src));

        void* ptr = dst.alloc();
        REQUIRE(ptr != nullptr);
        dst.free(ptr);

        REQUIRE(dst.get_free_space() == 128 * 5);
    }
}

TEST_CASE("Pool: Move assignment", "[pool][move]")
{
    SECTION("Move assignment transfers ownership")
    {
        AL::pool<> src(64, 10);
        AL::pool<> dst(128, 5);

        size_t src_capacity = src.get_capacity();
        size_t src_block_size = src.get_block_size();
        size_t src_free = src.get_free_space();

        dst = std::move(src);

        REQUIRE(dst.get_capacity() == src_capacity);
        REQUIRE(dst.get_block_size() == src_block_size);
        REQUIRE(dst.get_free_space() == src_free);
    }

    SECTION("Self move assignment is safe")
    {
        AL::pool<> p(64, 10);
        void* ptr = p.alloc();
        REQUIRE(ptr != nullptr);

        AL::pool<>& ref = p;
        p = std::move(ref);

        // Pool should still work after self-move
        p.free(ptr);
        REQUIRE(p.get_free_space() == 64 * 10);
    }

    SECTION("Moved-to pool replaces old memory")
    {
        AL::pool<> dst(32, 20);
        AL::pool<> src(256, 5);

        dst = std::move(src);

        REQUIRE(dst.get_block_size() == 256);
        REQUIRE(dst.get_block_count() == 5);

        void* ptr = dst.alloc();
        REQUIRE(ptr != nullptr);
        dst.free(ptr);
    }
}

TEST_CASE("Pool: Block size power-of-2 rounding", "[pool][sizes]")
{
    SECTION("Exact powers of 2 are unchanged")
    {
        AL::pool<> p8(8, 1);
        AL::pool<> p16(16, 1);
        AL::pool<> p64(64, 1);
        AL::pool<> p1024(1024, 1);

        REQUIRE(p8.get_block_size() == 8);
        REQUIRE(p16.get_block_size() == 16);
        REQUIRE(p64.get_block_size() == 64);
        REQUIRE(p1024.get_block_size() == 1024);
    }

    SECTION("Non-powers round up to next power of 2")
    {
        AL::pool<> p9(9, 1);
        AL::pool<> p33(33, 1);
        AL::pool<> p100(100, 1);
        AL::pool<> p500(500, 1);

        REQUIRE(p9.get_block_size() == 16);
        REQUIRE(p33.get_block_size() == 64);
        REQUIRE(p100.get_block_size() == 128);
        REQUIRE(p500.get_block_size() == 512);
    }

    SECTION("Below pointer size rounds to pointer size first")
    {
        AL::pool<> p1(1, 1);
        AL::pool<> p3(3, 1);

        REQUIRE(p1.get_block_size() == sizeof(void*));
        REQUIRE(p3.get_block_size() == sizeof(void*));
    }
}

TEST_CASE("Pool: Alloc exhaustion then reset then reuse", "[pool][reset][edge]")
{
    AL::pool<> p(64, 10);

    // Exhaust
    for (int i = 0; i < 10; ++i)
        p.alloc();
    REQUIRE(p.alloc() == nullptr);

    // Reset
    p.reset();
    REQUIRE(p.get_free_space() == 64 * 10);

    // Fully allocate again — all unique
    std::set<void*> ptrs;
    for (int i = 0; i < 10; ++i)
    {
        void* ptr = p.alloc();
        REQUIRE(ptr != nullptr);
        ptrs.insert(ptr);
    }
    REQUIRE(ptrs.size() == 10);
    REQUIRE(p.alloc() == nullptr);
}

