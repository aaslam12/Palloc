#include "dynamic_slab.h"
#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace AL;

TEST_CASE("Dynamic slab: basic allocation and free", "[dynamic_slab]")
{
    dynamic_slab ds(1.0);

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
}

TEST_CASE("Dynamic slab: grows when exhausted", "[dynamic_slab]")
{
    dynamic_slab ds(0.01); // tiny initial capacity

    SECTION("Allocating beyond capacity creates new slab")
    {
        std::vector<void*> ptrs;
        for (size_t i = 0; i < 1000; ++i)
        {
            void* p = ds.palloc(16);
            REQUIRE(p != nullptr);
            ptrs.push_back(p);
        }
        REQUIRE(ds.get_slab_count() > 1);
        for (void* p : ptrs)
            ds.free(p, 16);
    }
}

TEST_CASE("Dynamic slab: calloc returns zeroed memory", "[dynamic_slab]")
{
    dynamic_slab ds(1.0);

    SECTION("Calloc allocates and zeros")
    {
        char* p = static_cast<char*>(ds.calloc(64));
        REQUIRE(p != nullptr);
        for (size_t i = 0; i < 64; ++i)
            REQUIRE(p[i] == 0);
        ds.free(p, 64);
    }
}

TEST_CASE("Dynamic slab: tracks capacity and free space", "[dynamic_slab]")
{
    dynamic_slab ds(1.0);

    SECTION("Capacity increases with slabs")
    {
        size_t cap1 = ds.get_total_capacity();
        std::vector<void*> ptrs;
        for (size_t i = 0; i < 2000; ++i)
        {
            void* p = ds.palloc(8);
            if (p)
                ptrs.push_back(p);
        }
        size_t cap2 = ds.get_total_capacity();
        REQUIRE(cap2 > cap1);
        for (void* p : ptrs)
            ds.free(p, 8);
    }
}

TEST_CASE("Dynamic slab: different size classes", "[dynamic_slab]")
{
    dynamic_slab ds(1.0);

    SECTION("Allocate mixed sizes")
    {
        void* p8 = ds.palloc(8);
        void* p64 = ds.palloc(64);
        void* p512 = ds.palloc(512);
        void* p4096 = ds.palloc(4096);

        REQUIRE(p8 != nullptr);
        REQUIRE(p64 != nullptr);
        REQUIRE(p512 != nullptr);
        REQUIRE(p4096 != nullptr);

        ds.free(p8, 8);
        ds.free(p64, 64);
        ds.free(p512, 512);
        ds.free(p4096, 4096);
    }
}

TEST_CASE("Dynamic slab: free to correct slab", "[dynamic_slab]")
{
    dynamic_slab ds(0.01);

    SECTION("Allocations go to different slabs; free finds correct slab")
    {
        std::vector<void*> ptrs1, ptrs2;
        // Exhaust first slab
        for (size_t i = 0; i < 500 && ds.get_slab_count() == 1; ++i)
        {
            void* p = ds.palloc(16);
            if (p)
                ptrs1.push_back(p);
        }
        // Force creation of second slab
        for (size_t i = 0; i < 500 && ds.get_slab_count() == 2; ++i)
        {
            void* p = ds.palloc(16);
            if (p)
                ptrs2.push_back(p);
        }
        REQUIRE(ds.get_slab_count() >= 2);
        // Free from both slabs (shouldn't crash or corrupt)
        for (void* p : ptrs1)
            ds.free(p, 16);
        for (void* p : ptrs2)
            ds.free(p, 16);
    }
}

TEST_CASE("Dynamic slab: invalid sizes", "[dynamic_slab]")
{
    dynamic_slab ds(1.0);

    SECTION("Size 0 returns nullptr")
    {
        void* p = ds.palloc(0);
        REQUIRE(p == nullptr);
    }

    SECTION("Size > max returns nullptr")
    {
        void* p = ds.palloc(8192); // larger than max size class (4096)
        REQUIRE(p == nullptr);
    }

    SECTION("Free nullptr is safe")
    {
        ds.free(nullptr, 64);
    }

    SECTION("Free size 0 is safe")
    {
        ds.free(reinterpret_cast<void*>(0x1000), 0);
    }
}
