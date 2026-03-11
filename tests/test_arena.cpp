#include "arena.h"
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <unistd.h>
#include <vector>

const size_t PAGE_SIZE = getpagesize();

template<size_t A>
static void check_arena_valid(const AL::arena<A>& a)
{
    REQUIRE(a.get_used() == 0);
    REQUIRE(a.get_capacity() > 0);
}

TEST_CASE("Arena: Basic construction and initialization", "[arena][basic]")
{
    SECTION("Single page arena")
    {
        AL::arena a(PAGE_SIZE);
        REQUIRE(a.get_used() == 0);
        REQUIRE(a.get_capacity() >= PAGE_SIZE);
        REQUIRE(a.get_capacity() % PAGE_SIZE == 0);
    }

    SECTION("Multi-page arena")
    {
        AL::arena a(PAGE_SIZE * 3);
        REQUIRE(a.get_used() == 0);
        REQUIRE(a.get_capacity() >= PAGE_SIZE * 3);
        REQUIRE(a.get_capacity() % PAGE_SIZE == 0);
    }

    SECTION("Small arena (less than page size)")
    {
        AL::arena a(100);
        REQUIRE(a.get_used() == 0);
        REQUIRE(a.get_capacity() >= 100);
        REQUIRE(a.get_capacity() == PAGE_SIZE); // Should round up to page size
    }

    SECTION("Large arena")
    {
        AL::arena a(PAGE_SIZE * 100);
        REQUIRE(a.get_used() == 0);
        REQUIRE(a.get_capacity() >= PAGE_SIZE * 100);
    }
}

TEST_CASE("Arena: Basic allocations", "[arena][alloc]")
{
    AL::arena a(PAGE_SIZE);

    SECTION("Single small allocation")
    {
        void* ptr = a.alloc(64);
        REQUIRE(ptr != nullptr);
        REQUIRE(a.get_used() == 64);
    }

    SECTION("Multiple small allocations")
    {
        void* p1 = a.alloc(16);
        void* p2 = a.alloc(32);
        void* p3 = a.alloc(64);

        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);
        REQUIRE(p3 != nullptr);
        REQUIRE(a.get_used() == 112);

        // Verify pointers are distinct
        REQUIRE(p1 != p2);
        REQUIRE(p2 != p3);
        REQUIRE(p1 != p3);
    }

    SECTION("Sequential allocations are non-overlapping and aligned")
    {
        constexpr size_t alignment = alignof(std::max_align_t);
        char* p1 = static_cast<char*>(a.alloc(100));
        char* p2 = static_cast<char*>(a.alloc(100));
        char* p3 = static_cast<char*>(a.alloc(100));

        REQUIRE(reinterpret_cast<uintptr_t>(p1) % alignment == 0);
        REQUIRE(reinterpret_cast<uintptr_t>(p2) % alignment == 0);
        REQUIRE(reinterpret_cast<uintptr_t>(p3) % alignment == 0);

        // Allocations must not overlap
        REQUIRE(p2 >= p1 + 100);
        REQUIRE(p3 >= p2 + 100);
    }
}

TEST_CASE("Arena: Zero-size allocation", "[arena][alloc][edge]")
{
    AL::arena a(PAGE_SIZE);

    void* ptr = a.alloc(0);
    REQUIRE(ptr == nullptr);
    REQUIRE(a.get_used() == 0);
}

TEST_CASE("Arena: Full capacity allocation", "[arena][alloc][edge]")
{
    AL::arena a(PAGE_SIZE);

    SECTION("Allocate exactly full capacity")
    {
        void* ptr = a.alloc(a.get_capacity());
        REQUIRE(ptr != nullptr);
        REQUIRE(a.get_used() == a.get_capacity());
    }

    SECTION("Cannot allocate after full")
    {
        void* p1 = a.alloc(a.get_capacity());
        REQUIRE(p1 != nullptr);

        void* p2 = a.alloc(1);
        REQUIRE(p2 == nullptr);
        // fetch_add-based arena: failed allocs still bump 'used' past capacity
        REQUIRE(a.get_used() >= a.get_capacity());
    }
}

TEST_CASE("Arena: Calloc zeros memory", "[arena][calloc]")
{
    AL::arena a(PAGE_SIZE);

    SECTION("Calloc single allocation")
    {
        char* ptr = static_cast<char*>(a.calloc(100));
        REQUIRE(ptr != nullptr);

        for (size_t i = 0; i < 100; ++i)
        {
            REQUIRE(ptr[i] == 0);
        }
    }

    SECTION("Calloc after dirty memory")
    {
        // Allocate and dirty memory
        char* dirty = static_cast<char*>(a.alloc(100));
        std::memset(dirty, 0xFF, 100);

        // Reset and calloc
        a.reset();
        char* clean = static_cast<char*>(a.calloc(100));
        REQUIRE(clean != nullptr);

        for (size_t i = 0; i < 100; ++i)
        {
            REQUIRE(clean[i] == 0);
        }
    }

    SECTION("Multiple calloc allocations")
    {
        char* p1 = static_cast<char*>(a.calloc(50));
        char* p2 = static_cast<char*>(a.calloc(50));

        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);

        for (size_t i = 0; i < 50; ++i)
        {
            REQUIRE(p1[i] == 0);
            REQUIRE(p2[i] == 0);
        }
    }
}

TEST_CASE("Arena: Reset functionality", "[arena][reset]")
{
    AL::arena a(PAGE_SIZE);

    SECTION("Reset empty arena")
    {
        int result = a.reset();
        REQUIRE(result == 0);
        REQUIRE(a.get_used() == 0);
        REQUIRE(a.get_capacity() > 0);
    }

    SECTION("Reset after allocations")
    {
        void* p1 = a.alloc(100);
        void* p2 = a.alloc(200);
        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);
        REQUIRE(a.get_used() >= 300); // at least the sum of requested bytes, plus any alignment padding

        int result = a.reset();
        REQUIRE(result == 0);
        REQUIRE(a.get_used() == 0);
        REQUIRE(a.get_capacity() > 0);
    }

    SECTION("Allocate after reset")
    {
        void* p1 = a.alloc(500);
        REQUIRE(p1 != nullptr);

        a.reset();

        void* p2 = a.alloc(500);
        REQUIRE(p2 != nullptr);
        REQUIRE(p2 == p1); // Should get same memory location
    }

    SECTION("Multiple resets")
    {
        for (int i = 0; i < 10; ++i)
        {
            a.alloc(100);
            int result = a.reset();
            REQUIRE(result == 0);
            REQUIRE(a.get_used() == 0);
        }
    }

    SECTION("Reset full arena")
    {
        void* ptr = a.alloc(a.get_capacity());
        REQUIRE(ptr != nullptr);
        REQUIRE(a.get_used() == a.get_capacity());

        a.reset();
        REQUIRE(a.get_used() == 0);

        // Should be able to allocate again
        ptr = a.alloc(100);
        REQUIRE(ptr != nullptr);
    }
}

TEST_CASE("Arena: Memory integrity", "[arena][integrity]")
{
    AL::arena a(PAGE_SIZE * 2);

    SECTION("Write and read back data")
    {
        struct TestData
        {
            int x;
            double y;
            char z[16];
        };

        TestData* data = static_cast<TestData*>(a.alloc(sizeof(TestData)));
        REQUIRE(data != nullptr);

        data->x = 42;
        data->y = 3.14159;
        std::strcpy(data->z, "test");

        REQUIRE(data->x == 42);
        REQUIRE(data->y == 3.14159);
        REQUIRE(std::strcmp(data->z, "test") == 0);
    }

    SECTION("Multiple allocations don't overlap")
    {
        int* arr1 = static_cast<int*>(a.alloc(sizeof(int) * 10));
        int* arr2 = static_cast<int*>(a.alloc(sizeof(int) * 10));

        REQUIRE(arr1 != nullptr);
        REQUIRE(arr2 != nullptr);

        // Fill with distinct patterns
        for (int i = 0; i < 10; ++i)
        {
            arr1[i] = i;
            arr2[i] = i + 100;
        }

        // Verify no corruption
        for (int i = 0; i < 10; ++i)
        {
            REQUIRE(arr1[i] == i);
            REQUIRE(arr2[i] == i + 100);
        }
    }

    SECTION("Large allocation integrity")
    {
        size_t size = PAGE_SIZE;
        char* buffer = static_cast<char*>(a.alloc(size));
        REQUIRE(buffer != nullptr);

        // Write pattern
        for (size_t i = 0; i < size; ++i)
        {
            buffer[i] = static_cast<char>(i % 256);
        }

        // Verify pattern
        for (size_t i = 0; i < size; ++i)
        {
            REQUIRE(buffer[i] == static_cast<char>(i % 256));
        }
    }
}

TEST_CASE("Arena: Capacity rounding", "[arena][capacity]")
{
    SECTION("Non-page-aligned sizes round up")
    {
        AL::arena a1(PAGE_SIZE + 1);
        REQUIRE(a1.get_capacity() == PAGE_SIZE * 2);

        AL::arena a2(PAGE_SIZE * 2 + 100);
        REQUIRE(a2.get_capacity() == PAGE_SIZE * 3);
    }

    SECTION("Page-aligned sizes don't change")
    {
        AL::arena a1(PAGE_SIZE);
        REQUIRE(a1.get_capacity() == PAGE_SIZE);

        AL::arena a2(PAGE_SIZE * 5);
        REQUIRE(a2.get_capacity() == PAGE_SIZE * 5);
    }
}

