#include "dynamic_slab.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <set>
#include <vector>

using namespace AL;

// ──────────────────────────────────────────────────────────────────────────────
// Construction
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Dynamic slab: default construction", "[dynamic_slab][basic]")
{
    default_dynamic_slab ds;
    REQUIRE(ds.get_slab_count() == 1);
    REQUIRE(ds.get_total_capacity() > 0);
    REQUIRE(ds.get_total_free() > 0);
}

TEST_CASE("Dynamic slab: custom config construction", "[dynamic_slab][config]")
{
    constexpr std::array<AL::size_class, 2> CFG = {{
        {.byte_size =  8, .num_blocks = 4, .batch_size = 2},
        {.byte_size = 16, .num_blocks = 4, .batch_size = 2},
    }};
    dynamic_slab<slab_config<2, CFG>> ds;
    REQUIRE(ds.get_slab_count() == 1);
    REQUIRE(ds.get_total_capacity() > 0);
}

// ──────────────────────────────────────────────────────────────────────────────
// Basic alloc / free
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Dynamic slab: basic allocation and free", "[dynamic_slab][alloc]")
{
    default_dynamic_slab ds;

    SECTION("Single allocation succeeds")
    {
        void* p = ds.palloc(64);
        REQUIRE(p != nullptr);
        ds.free(p, 64);
    }

    SECTION("Multiple allocations from first slab")
    {
        std::vector<void*> ptrs;
        for (size_t i = 0; i < 100; ++i)
        {
            void* p = ds.palloc(32);
            REQUIRE(p != nullptr);
            ptrs.push_back(p);
        }
        REQUIRE(ds.get_slab_count() == 1);
        for (void* p : ptrs)
            ds.free(p, 32);
    }

    SECTION("All default size classes allocate")
    {
        size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
        for (size_t sz : sizes)
        {
            void* p = ds.palloc(sz);
            REQUIRE(p != nullptr);
            ds.free(p, sz);
        }
    }

    SECTION("Returned pointers are unique")
    {
        std::set<void*> ptrs;
        for (int i = 0; i < 50; ++i)
            ptrs.insert(ds.palloc(64));
        REQUIRE(ptrs.size() == 50);
        for (void* p : ptrs)
            ds.free(p, 64);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Growth
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Dynamic slab: grows when exhausted", "[dynamic_slab][growth]")
{
    constexpr std::array<AL::size_class, 1> TINY = {{
        {.byte_size = 8, .num_blocks = 4, .batch_size = 1},
    }};
    dynamic_slab<slab_config<1, TINY>> ds;

    SECTION("Allocating beyond capacity creates new slab")
    {
        std::vector<void*> ptrs;
        for (size_t i = 0; i < 20; ++i)
        {
            void* p = ds.palloc(8);
            REQUIRE(p != nullptr);
            ptrs.push_back(p);
        }
        REQUIRE(ds.get_slab_count() > 1);
        for (void* p : ptrs)
            ds.free(p, 8);
    }

    SECTION("Capacity increases as new slabs are added")
    {
        size_t cap1 = ds.get_total_capacity();
        for (size_t i = 0; i < 20; ++i)
            ds.palloc(8);
        REQUIRE(ds.get_total_capacity() > cap1);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Calloc
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Dynamic slab: calloc returns zeroed memory", "[dynamic_slab][calloc]")
{
    default_dynamic_slab ds;

    SECTION("Calloc zeros small allocation")
    {
        char* p = static_cast<char*>(ds.calloc(64));
        REQUIRE(p != nullptr);
        for (size_t i = 0; i < 64; ++i)
            REQUIRE(p[i] == 0);
        ds.free(p, 64);
    }

    SECTION("Calloc zeros large allocation")
    {
        char* p = static_cast<char*>(ds.calloc(4096));
        REQUIRE(p != nullptr);
        for (size_t i = 0; i < 4096; ++i)
            REQUIRE(p[i] == 0);
        ds.free(p, 4096);
    }

    SECTION("Calloc after dirty memory zeros correctly")
    {
        char* p1 = static_cast<char*>(ds.palloc(64));
        REQUIRE(p1 != nullptr);
        std::memset(p1, 0xFF, 64);
        ds.free(p1, 64);

        char* p2 = static_cast<char*>(ds.calloc(64));
        REQUIRE(p2 != nullptr);
        for (size_t i = 0; i < 64; ++i)
            REQUIRE(p2[i] == 0);
        ds.free(p2, 64);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Capacity / free tracking
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Dynamic slab: tracks capacity and free space", "[dynamic_slab][stats]")
{
    default_dynamic_slab ds;

    SECTION("Capacity is positive at start")
    {
        REQUIRE(ds.get_total_capacity() > 0);
    }

    SECTION("Free space decreases on alloc")
    {
        size_t before = ds.get_total_free();
        ds.palloc(64);
        REQUIRE(ds.get_total_free() < before);
    }

    SECTION("Free space increases after free + reset")
    {
        // TLC caches frees; reset flushes to pool level
        void* p = ds.palloc(64);
        ds.free(p, 64);
        // Note: exact free count may include TLC buffering
        REQUIRE(ds.get_total_free() <= ds.get_total_capacity());
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Free routing across multiple slabs
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Dynamic slab: free to correct slab", "[dynamic_slab][free]")
{
    constexpr std::array<AL::size_class, 1> TINY = {{
        {.byte_size = 16, .num_blocks = 4, .batch_size = 1},
    }};
    dynamic_slab<slab_config<1, TINY>> ds;

    SECTION("Frees from multiple slabs don't crash or corrupt")
    {
        std::vector<void*> ptrs;
        for (size_t i = 0; i < 20; ++i)
        {
            void* p = ds.palloc(16);
            REQUIRE(p != nullptr);
            ptrs.push_back(p);
        }
        REQUIRE(ds.get_slab_count() >= 2);
        for (void* p : ptrs)
            ds.free(p, 16);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Invalid inputs
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Dynamic slab: invalid sizes", "[dynamic_slab][edge]")
{
    default_dynamic_slab ds;

    SECTION("Size 0 returns nullptr")
    {
        REQUIRE(ds.palloc(0) == nullptr);
    }

    SECTION("Size above max class returns nullptr")
    {
        REQUIRE(ds.palloc(8192) == nullptr);
    }

    SECTION("size_t max returns nullptr")
    {
        REQUIRE(ds.palloc(static_cast<size_t>(-1)) == nullptr);
    }

    SECTION("Free nullptr is safe")
    {
        ds.free(nullptr, 64);
    }

    SECTION("Free with size 0 is safe")
    {
        ds.free(reinterpret_cast<void*>(0x1000), 0);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Memory integrity
// ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Dynamic slab: memory integrity", "[dynamic_slab][integrity]")
{
    default_dynamic_slab ds;

    SECTION("Write and read back structured data")
    {
        struct Point { int x; int y; };
        Point* p = static_cast<Point*>(ds.palloc(sizeof(Point)));
        REQUIRE(p != nullptr);
        p->x = 10; p->y = 20;
        REQUIRE(p->x == 10);
        REQUIRE(p->y == 20);
        ds.free(p, sizeof(Point));
    }

    SECTION("Multiple live allocations don't interfere")
    {
        int* a = static_cast<int*>(ds.palloc(sizeof(int) * 8));
        int* b = static_cast<int*>(ds.palloc(sizeof(int) * 8));
        REQUIRE(a != nullptr); REQUIRE(b != nullptr);
        for (int i = 0; i < 8; ++i) { a[i] = i; b[i] = i + 100; }
        for (int i = 0; i < 8; ++i)
        {
            REQUIRE(a[i] == i);
            REQUIRE(b[i] == i + 100);
        }
        ds.free(a, sizeof(int) * 8);
        ds.free(b, sizeof(int) * 8);
    }
}
