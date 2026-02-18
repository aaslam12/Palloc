#include "slab.h"
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <set>
#include <vector>

// ============================================================================
// Construction and Initialization
// ============================================================================

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

TEST_CASE("Slab: Pool statistics", "[slab][stats]")
{
    AL::slab s;

    SECTION("Pool count matches size class count")
    {
        REQUIRE(s.get_pool_count() == 10);
    }

    SECTION("Block sizes are in ascending order")
    {
        for (size_t i = 1; i < s.get_pool_count(); ++i)
        {
            REQUIRE(s.get_pool_block_size(i) > s.get_pool_block_size(i - 1));
        }
    }

    SECTION("All block sizes are powers of two")
    {
        for (size_t i = 0; i < s.get_pool_count(); ++i)
        {
            size_t bs = s.get_pool_block_size(i);
            REQUIRE((bs & (bs - 1)) == 0);
        }
    }

    SECTION("Expected size classes are present")
    {
        REQUIRE(s.get_pool_block_size(0) == 8);
        REQUIRE(s.get_pool_block_size(1) == 16);
        REQUIRE(s.get_pool_block_size(2) == 32);
        REQUIRE(s.get_pool_block_size(3) == 64);
        REQUIRE(s.get_pool_block_size(4) == 128);
        REQUIRE(s.get_pool_block_size(5) == 256);
        REQUIRE(s.get_pool_block_size(6) == 512);
        REQUIRE(s.get_pool_block_size(7) == 1024);
        REQUIRE(s.get_pool_block_size(8) == 2048);
        REQUIRE(s.get_pool_block_size(9) == 4096);
    }

    SECTION("Each pool has free space initially")
    {
        for (size_t i = 0; i < s.get_pool_count(); ++i)
        {
            REQUIRE(s.get_pool_free_space(i) > 0);
        }
    }

    SECTION("Invalid pool index returns 0")
    {
        REQUIRE(s.get_pool_block_size(999) == 0);
        REQUIRE(s.get_pool_free_space(999) == 0);
    }
}

// ============================================================================
// Basic Allocation
// ============================================================================

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

// ============================================================================
// Edge Cases: Allocation
// ============================================================================

TEST_CASE("Slab: Zero-size allocation", "[slab][alloc][edge]")
{
    AL::slab s;
    REQUIRE(s.alloc(0) == nullptr);
}

TEST_CASE("Slab: Over-sized allocation", "[slab][alloc][edge]")
{
    AL::slab s;

    SECTION("Just above max size class")
    {
        REQUIRE(s.alloc(4097) == nullptr);
    }

    SECTION("Very large size")
    {
        REQUIRE(s.alloc(1024 * 1024) == nullptr);
    }

    SECTION("SIZE_MAX")
    {
        REQUIRE(s.alloc((size_t)-1) == nullptr);
    }
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

// ============================================================================
// Calloc
// ============================================================================

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

TEST_CASE("Slab: Calloc edge cases", "[slab][calloc][edge]")
{
    AL::slab s;

    SECTION("Zero-size calloc")
    {
        REQUIRE(s.calloc(0) == nullptr);
    }

    SECTION("Over-sized calloc")
    {
        REQUIRE(s.calloc(4097) == nullptr);
    }
}

// ============================================================================
// Free
// ============================================================================

TEST_CASE("Slab: Basic free", "[slab][free]")
{
    AL::slab s;

    SECTION("Free single allocation increases free space")
    {
        void* ptr = s.alloc(64);
        REQUIRE(ptr != nullptr);

        size_t free_before = s.get_total_free();
        s.free(ptr, 64);
        size_t free_after = s.get_total_free();

        REQUIRE(free_after > free_before);
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

TEST_CASE("Slab: Free order independence", "[slab][free]")
{
    AL::slab s;

    SECTION("LIFO order")
    {
        std::vector<void*> ptrs;
        for (int i = 0; i < 10; ++i)
            ptrs.push_back(s.alloc(64));

        for (auto it = ptrs.rbegin(); it != ptrs.rend(); ++it)
            s.free(*it, 64);

        REQUIRE(s.alloc(64) != nullptr);
    }

    SECTION("FIFO order")
    {
        std::vector<void*> ptrs;
        for (int i = 0; i < 10; ++i)
            ptrs.push_back(s.alloc(64));

        for (void* ptr : ptrs)
            s.free(ptr, 64);

        REQUIRE(s.alloc(64) != nullptr);
    }

    SECTION("Mixed sizes in arbitrary order")
    {
        void* p32 = s.alloc(32);
        void* p64 = s.alloc(64);
        void* p128 = s.alloc(128);
        void* p32_2 = s.alloc(32);

        s.free(p64, 64);
        s.free(p32, 32);
        s.free(p32_2, 32);
        s.free(p128, 128);

        REQUIRE(s.alloc(32) != nullptr);
        REQUIRE(s.alloc(64) != nullptr);
        REQUIRE(s.alloc(128) != nullptr);
    }
}

// ============================================================================
// Reset
// ============================================================================

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

// ============================================================================
// Memory Integrity
// ============================================================================

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

// ============================================================================
// Pointer Uniqueness
// ============================================================================

TEST_CASE("Slab: Pointer uniqueness", "[slab][unique]")
{
    AL::slab s;

    SECTION("All pointers unique within same size")
    {
        std::set<void*> ptrs;
        for (int i = 0; i < 50; ++i)
        {
            void* ptr = s.alloc(64);
            REQUIRE(ptr != nullptr);
            auto result = ptrs.insert(ptr);
            REQUIRE(result.second == true);
        }
    }

    SECTION("All pointers unique across different sizes")
    {
        std::set<void*> ptrs;
        size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024};

        for (size_t size : sizes)
        {
            for (int i = 0; i < 5; ++i)
            {
                void* ptr = s.alloc(size);
                REQUIRE(ptr != nullptr);
                auto result = ptrs.insert(ptr);
                REQUIRE(result.second == true);
            }
        }
    }
}

// ============================================================================
// Allocation Patterns
// ============================================================================

TEST_CASE("Slab: Alloc/free patterns", "[slab][pattern]")
{
    AL::slab s;

    SECTION("Alternating alloc/free same size")
    {
        size_t initial_free = s.get_total_free();

        for (int i = 0; i < 100; ++i)
        {
            void* ptr = s.alloc(64);
            REQUIRE(ptr != nullptr);
            s.free(ptr, 64);
        }

        REQUIRE(s.get_total_free() == initial_free);
    }

    SECTION("Alternating alloc/free different sizes")
    {
        size_t initial_free = s.get_total_free();

        for (int i = 0; i < 50; ++i)
        {
            void* p32 = s.alloc(32);
            void* p128 = s.alloc(128);

            s.free(p32, 32);
            s.free(p128, 128);
        }

        REQUIRE(s.get_total_free() == initial_free);
    }

    SECTION("Batch alloc then batch free")
    {
        size_t initial_free = s.get_total_free();

        std::vector<void*> ptrs;
        for (int i = 0; i < 50; ++i)
        {
            void* ptr = s.alloc(64);
            REQUIRE(ptr != nullptr);
            ptrs.push_back(ptr);
        }

        for (void* ptr : ptrs)
            s.free(ptr, 64);

        REQUIRE(s.get_total_free() == initial_free);
    }

    SECTION("Multiple sizes batch alloc/free")
    {
        size_t initial_free = s.get_total_free();

        std::vector<std::pair<void*, size_t>> ptrs;
        size_t sizes[] = {32, 64, 128, 256, 512};

        for (int i = 0; i < 50; ++i)
        {
            size_t size = sizes[i % 5];
            void* ptr = s.alloc(size);
            REQUIRE(ptr != nullptr);
            ptrs.push_back({ptr, size});
        }

        for (auto& [ptr, size] : ptrs)
            s.free(ptr, size);

        REQUIRE(s.get_total_free() == initial_free);
    }
}

TEST_CASE("Slab: Use all pools simultaneously", "[slab][pattern]")
{
    AL::slab s;

    std::vector<std::pair<void*, size_t>> allocations;
    size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

    for (size_t size : sizes)
    {
        void* ptr = s.alloc(size);
        REQUIRE(ptr != nullptr);
        allocations.push_back({ptr, size});
    }

    for (auto& [ptr, size] : allocations)
        s.free(ptr, size);
}

// ============================================================================
// Free space tracking
// ============================================================================

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
        void* ptr = s.alloc(64);
        size_t before = s.get_total_free();
        s.free(ptr, 64);
        REQUIRE(s.get_total_free() > before);
    }

    SECTION("Pool-specific free space decreases on alloc")
    {
        // The 64-byte size class is at index 3
        size_t before = s.get_pool_free_space(3);
        s.alloc(64);
        REQUIRE(s.get_pool_free_space(3) < before);
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

// ============================================================================
// Reuse Patterns
// ============================================================================

TEST_CASE("Slab: Reuse patterns", "[slab][reuse]")
{
    SECTION("LIFO reuse")
    {
        AL::slab s;

        void* p1 = s.alloc(64);
        void* p2 = s.alloc(64);
        s.free(p2, 64);
        s.free(p1, 64);

        void* p3 = s.alloc(64);
        void* p4 = s.alloc(64);
        REQUIRE(p3 != nullptr);
        REQUIRE(p4 != nullptr);
    }

    SECTION("Reuse after reset")
    {
        AL::slab s;

        void* p1 = s.alloc(64);
        REQUIRE(p1 != nullptr);

        s.reset();

        void* p2 = s.alloc(64);
        REQUIRE(p2 != nullptr);
    }

    SECTION("Exhaust, free all, allocate again")
    {
        AL::slab s(0.01);

        std::vector<void*> ptrs;
        while (true)
        {
            void* ptr = s.alloc(64);
            if (ptr == nullptr)
                break;
            ptrs.push_back(ptr);
        }

        size_t count = ptrs.size();
        REQUIRE(count > 0);

        for (void* ptr : ptrs)
            s.free(ptr, 64);

        // Should be able to allocate the same count again
        for (size_t i = 0; i < count; ++i)
        {
            void* ptr = s.alloc(64);
            REQUIRE(ptr != nullptr);
        }
    }
}

// ============================================================================
// Edge Case: Single-byte allocation
// ============================================================================

TEST_CASE("Slab: Single byte allocation", "[slab][edge]")
{
    AL::slab s;

    void* ptr = s.alloc(1);
    REQUIRE(ptr != nullptr);

    // Should be usable
    *static_cast<char*>(ptr) = 'A';
    REQUIRE(*static_cast<char*>(ptr) == 'A');

    s.free(ptr, 1);
}

// ============================================================================
// Edge Case: Max supported size
// ============================================================================

TEST_CASE("Slab: Max size class boundary", "[slab][edge]")
{
    AL::slab s;

    SECTION("Exactly max size succeeds")
    {
        void* ptr = s.alloc(4096);
        REQUIRE(ptr != nullptr);
        s.free(ptr, 4096);
    }

    SECTION("One above max size fails")
    {
        REQUIRE(s.alloc(4097) == nullptr);
    }
}

// ============================================================================
// Edge Case: Free with mismatched size
// ============================================================================

TEST_CASE("Slab: Free edge cases", "[slab][free][edge]")
{
    AL::slab s;

    SECTION("Free with SIZE_MAX is safe")
    {
        s.free(nullptr, (size_t)-1);
    }

    SECTION("Free nullptr with various sizes")
    {
        s.free(nullptr, 0);
        s.free(nullptr, 32);
        s.free(nullptr, 4096);
        s.free(nullptr, 99999);
        // No crash = pass
    }
}
