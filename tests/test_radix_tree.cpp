#include "radix_tree.h"
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

static void* addr(uintptr_t v)
{
    return reinterpret_cast<void*>(v);
}

TEST_CASE("RadixTree: Construction and destruction", "[radix_tree][basic]")
{
    SECTION("Default construction does not crash")
    {
        AL::radix_tree rt;
    }

    SECTION("Lookup on empty tree returns 0")
    {
        AL::radix_tree rt;
        REQUIRE(rt.lookup(addr(0x1000)) == 0);
        REQUIRE(rt.lookup(addr(0xDEADBEEF)) == 0);
        REQUIRE(rt.lookup(nullptr) == 0);
    }
}

TEST_CASE("RadixTree: Single range insert and lookup", "[radix_tree][basic]")
{
    AL::radix_tree rt;
    rt.insert(addr(0x1000), addr(0x2000), 1);

    SECTION("Lookup at start of range")
    {
        REQUIRE(rt.lookup(addr(0x1000)) == 1);
    }

    SECTION("Lookup in middle of range")
    {
        REQUIRE(rt.lookup(addr(0x1500)) == 1);
        REQUIRE(rt.lookup(addr(0x1800)) == 1);
        REQUIRE(rt.lookup(addr(0x1FFF)) == 1);
    }

    SECTION("Lookup at end boundary returns 0 (end is exclusive)")
    {
        REQUIRE(rt.lookup(addr(0x2000)) == 0);
    }

    SECTION("Lookup before range returns 0")
    {
        REQUIRE(rt.lookup(addr(0x0FFF)) == 0);
        REQUIRE(rt.lookup(addr(0x0500)) == 0);
    }

    SECTION("Lookup after range returns 0")
    {
        REQUIRE(rt.lookup(addr(0x2001)) == 0);
        REQUIRE(rt.lookup(addr(0x5000)) == 0);
    }
}

TEST_CASE("RadixTree: Multiple non-overlapping ranges", "[radix_tree][multi]")
{
    AL::radix_tree rt;
    rt.insert(addr(0x1000), addr(0x2000), 1);
    rt.insert(addr(0x3000), addr(0x4000), 2);
    rt.insert(addr(0x5000), addr(0x6000), 3);

    SECTION("Each range returns correct slab_id")
    {
        REQUIRE(rt.lookup(addr(0x1500)) == 1);
        REQUIRE(rt.lookup(addr(0x3500)) == 2);
        REQUIRE(rt.lookup(addr(0x5500)) == 3);
    }

    SECTION("Gaps between ranges return 0")
    {
        REQUIRE(rt.lookup(addr(0x2500)) == 0);
        REQUIRE(rt.lookup(addr(0x4500)) == 0);
    }

    SECTION("Boundaries are correct for all ranges")
    {
        REQUIRE(rt.lookup(addr(0x1000)) == 1);
        REQUIRE(rt.lookup(addr(0x1FFF)) == 1);
        REQUIRE(rt.lookup(addr(0x2000)) == 0);

        REQUIRE(rt.lookup(addr(0x3000)) == 2);
        REQUIRE(rt.lookup(addr(0x3FFF)) == 2);
        REQUIRE(rt.lookup(addr(0x4000)) == 0);

        REQUIRE(rt.lookup(addr(0x5000)) == 3);
        REQUIRE(rt.lookup(addr(0x5FFF)) == 3);
        REQUIRE(rt.lookup(addr(0x6000)) == 0);
    }
}

TEST_CASE("RadixTree: Adjacent ranges", "[radix_tree][multi]")
{
    AL::radix_tree rt;
    rt.insert(addr(0x1000), addr(0x2000), 1);
    rt.insert(addr(0x2000), addr(0x3000), 2);

    SECTION("Each side of the boundary returns correct slab_id")
    {
        REQUIRE(rt.lookup(addr(0x1FFF)) == 1);
        REQUIRE(rt.lookup(addr(0x2000)) == 2);
    }

    SECTION("Full range coverage")
    {
        REQUIRE(rt.lookup(addr(0x1000)) == 1);
        REQUIRE(rt.lookup(addr(0x1500)) == 1);
        REQUIRE(rt.lookup(addr(0x2500)) == 2);
        REQUIRE(rt.lookup(addr(0x2FFF)) == 2);
    }

    SECTION("Outside returns 0")
    {
        REQUIRE(rt.lookup(addr(0x0FFF)) == 0);
        REQUIRE(rt.lookup(addr(0x3000)) == 0);
    }
}

TEST_CASE("RadixTree: Single page range", "[radix_tree][edge]")
{
    AL::radix_tree rt;
    rt.insert(addr(0xAB000), addr(0xAC000), 42);

    REQUIRE(rt.lookup(addr(0xAB000)) == 42);
    REQUIRE(rt.lookup(addr(0xAB500)) == 42);
    REQUIRE(rt.lookup(addr(0xABFFF)) == 42);
    REQUIRE(rt.lookup(addr(0xAAFFF)) == 0);
    REQUIRE(rt.lookup(addr(0xAC000)) == 0);
}

TEST_CASE("RadixTree: Large range", "[radix_tree][edge]")
{
    AL::radix_tree rt;
    rt.insert(addr(0x10000000), addr(0x20000000), 7);

    REQUIRE(rt.lookup(addr(0x10000000)) == 7);
    REQUIRE(rt.lookup(addr(0x15000000)) == 7);
    REQUIRE(rt.lookup(addr(0x1FFFFFFF)) == 7);
    REQUIRE(rt.lookup(addr(0x20000000)) == 0);
    REQUIRE(rt.lookup(addr(0x0FFFFFFF)) == 0);
}

TEST_CASE("RadixTree: High userspace address values", "[radix_tree][edge]")
{
    AL::radix_tree rt;

    // Near top of 47-bit user-space canonical range
    uintptr_t high = 0x00007FFFFFF00000ULL;
    rt.insert(addr(high), addr(high + 0x10000), 99);

    REQUIRE(rt.lookup(addr(high)) == 99);
    REQUIRE(rt.lookup(addr(high + 0x5000)) == 99);
    REQUIRE(rt.lookup(addr(high + 0xFFFF)) == 99);
    REQUIRE(rt.lookup(addr(high + 0x10000)) == 0);
    REQUIRE(rt.lookup(addr(high - 1)) == 0);
}

TEST_CASE("RadixTree: Many ranges", "[radix_tree][multi]")
{
    AL::radix_tree rt;
    const size_t NUM_RANGES = 100;
    const uintptr_t RANGE_SIZE = 0x1000;
    const uintptr_t GAP = 0x1000;

    for (size_t i = 0; i < NUM_RANGES; ++i)
    {
        uintptr_t start = 0x100000 + i * (RANGE_SIZE + GAP);
        uintptr_t end = start + RANGE_SIZE;
        rt.insert(addr(start), addr(end), i + 1);
    }

    SECTION("All ranges return correct slab_id")
    {
        for (size_t i = 0; i < NUM_RANGES; ++i)
        {
            uintptr_t start = 0x100000 + i * (RANGE_SIZE + GAP);
            REQUIRE(rt.lookup(addr(start)) == i + 1);
            REQUIRE(rt.lookup(addr(start + RANGE_SIZE / 2)) == i + 1);
            REQUIRE(rt.lookup(addr(start + RANGE_SIZE - 1)) == i + 1);
        }
    }

    SECTION("Gaps return 0")
    {
        for (size_t i = 0; i < NUM_RANGES - 1; ++i)
        {
            uintptr_t gap_start = 0x100000 + i * (RANGE_SIZE + GAP) + RANGE_SIZE;
            REQUIRE(rt.lookup(addr(gap_start)) == 0);
            REQUIRE(rt.lookup(addr(gap_start + GAP / 2)) == 0);
        }
    }
}

TEST_CASE("RadixTree: Page-aligned heap blocks", "[radix_tree][heap]")
{
    AL::radix_tree rt;

    // Page-aligned allocation ensures clean page boundaries
    const size_t PAGE = 4096;
    void* big = std::aligned_alloc(PAGE, PAGE * 6);
    REQUIRE(big != nullptr);

    auto* base = static_cast<std::byte*>(big);
    void* block1 = base;              // [page 0, page 1)
    void* block2 = base + PAGE * 2;   // [page 2, page 3)  gap at page 1
    void* block3 = base + PAGE * 4;   // [page 4, page 5)  gap at page 3

    uintptr_t b1 = reinterpret_cast<uintptr_t>(block1);
    uintptr_t b2 = reinterpret_cast<uintptr_t>(block2);
    uintptr_t b3 = reinterpret_cast<uintptr_t>(block3);

    rt.insert(block1, addr(b1 + PAGE), 1);
    rt.insert(block2, addr(b2 + PAGE), 2);
    rt.insert(block3, addr(b3 + PAGE), 3);

    SECTION("Lookup at start of each block")
    {
        REQUIRE(rt.lookup(block1) == 1);
        REQUIRE(rt.lookup(block2) == 2);
        REQUIRE(rt.lookup(block3) == 3);
    }

    SECTION("Lookup at offsets within each block")
    {
        REQUIRE(rt.lookup(addr(b1 + 100)) == 1);
        REQUIRE(rt.lookup(addr(b2 + 2048)) == 2);
        REQUIRE(rt.lookup(addr(b3 + PAGE - 1)) == 3);
    }

    SECTION("Lookup in gaps between blocks returns 0")
    {
        REQUIRE(rt.lookup(addr(b1 + PAGE)) == 0);
        REQUIRE(rt.lookup(addr(b1 + PAGE + 100)) == 0);
        REQUIRE(rt.lookup(addr(b2 + PAGE)) == 0);
        REQUIRE(rt.lookup(addr(b2 + PAGE + 100)) == 0);
        REQUIRE(rt.lookup(addr(b3 + PAGE)) == 0);
    }

    std::free(big);
}

TEST_CASE("RadixTree: Distinct slab IDs are preserved", "[radix_tree][basic]")
{
    AL::radix_tree rt;
    rt.insert(addr(0x10000), addr(0x11000), 42);
    rt.insert(addr(0x20000), addr(0x21000), 999);
    rt.insert(addr(0x30000), addr(0x31000), 1);
    rt.insert(addr(0x40000), addr(0x41000), SIZE_MAX);

    REQUIRE(rt.lookup(addr(0x10500)) == 42);
    REQUIRE(rt.lookup(addr(0x20500)) == 999);
    REQUIRE(rt.lookup(addr(0x30500)) == 1);
    REQUIRE(rt.lookup(addr(0x40500)) == SIZE_MAX);
}

TEST_CASE("RadixTree: Addresses with varied byte patterns", "[radix_tree][edge]")
{
    AL::radix_tree rt;

    // Use addresses within 48-bit VA space with varied byte patterns
    rt.insert(addr(0x0000FF00FF000000), addr(0x0000FF00FF010000), 1);
    rt.insert(addr(0x00000F0F0F000000), addr(0x00000F0F0F010000), 2);

    REQUIRE(rt.lookup(addr(0x0000FF00FF000000)) == 1);
    REQUIRE(rt.lookup(addr(0x0000FF00FF005000)) == 1);
    REQUIRE(rt.lookup(addr(0x0000FF00FF00FFFF)) == 1);
    REQUIRE(rt.lookup(addr(0x00000F0F0F000000)) == 2);
    REQUIRE(rt.lookup(addr(0x00000F0F0F008000)) == 2);

    REQUIRE(rt.lookup(addr(0x0000FF00FF010000)) == 0);
    REQUIRE(rt.lookup(addr(0x00000F0F0F010000)) == 0);
}

TEST_CASE("RadixTree: Ranges sharing common prefix bytes", "[radix_tree][multi]")
{
    AL::radix_tree rt;
    rt.insert(addr(0xAABBCC001000), addr(0xAABBCC002000), 1);
    rt.insert(addr(0xAABBCC003000), addr(0xAABBCC004000), 2);
    rt.insert(addr(0xAABBDD001000), addr(0xAABBDD002000), 3);

    REQUIRE(rt.lookup(addr(0xAABBCC001500)) == 1);
    REQUIRE(rt.lookup(addr(0xAABBCC003500)) == 2);
    REQUIRE(rt.lookup(addr(0xAABBDD001500)) == 3);

    REQUIRE(rt.lookup(addr(0xAABBCC002500)) == 0);
    REQUIRE(rt.lookup(addr(0xAABBEE001500)) == 0);
}

TEST_CASE("RadixTree: Multiple trees are independent", "[radix_tree][basic]")
{
    AL::radix_tree rt1;
    AL::radix_tree rt2;

    rt1.insert(addr(0x1000), addr(0x2000), 10);
    rt2.insert(addr(0x1000), addr(0x2000), 20);

    REQUIRE(rt1.lookup(addr(0x1500)) == 10);
    REQUIRE(rt2.lookup(addr(0x1500)) == 20);

    REQUIRE(rt1.lookup(addr(0x3000)) == 0);
    REQUIRE(rt2.lookup(addr(0x3000)) == 0);
}

TEST_CASE("RadixTree: Insert after lookups", "[radix_tree][basic]")
{
    AL::radix_tree rt;

    REQUIRE(rt.lookup(addr(0x5000)) == 0);

    rt.insert(addr(0x5000), addr(0x6000), 5);
    REQUIRE(rt.lookup(addr(0x5000)) == 5);

    REQUIRE(rt.lookup(addr(0x7000)) == 0);

    rt.insert(addr(0x7000), addr(0x8000), 7);
    REQUIRE(rt.lookup(addr(0x7000)) == 7);

    REQUIRE(rt.lookup(addr(0x5500)) == 5);
}

TEST_CASE("RadixTree: Page-aligned ranges typical of slab usage", "[radix_tree][slab]")
{
    AL::radix_tree rt;
    const size_t PAGE = 4096;

    rt.insert(addr(PAGE * 10), addr(PAGE * 20), 1);
    rt.insert(addr(PAGE * 30), addr(PAGE * 40), 2);
    rt.insert(addr(PAGE * 50), addr(PAGE * 100), 3);

    REQUIRE(rt.lookup(addr(PAGE * 10)) == 1);
    REQUIRE(rt.lookup(addr(PAGE * 15)) == 1);
    REQUIRE(rt.lookup(addr(PAGE * 20 - 1)) == 1);
    REQUIRE(rt.lookup(addr(PAGE * 20)) == 0);

    REQUIRE(rt.lookup(addr(PAGE * 35)) == 2);

    REQUIRE(rt.lookup(addr(PAGE * 75)) == 3);
    REQUIRE(rt.lookup(addr(PAGE * 100 - 1)) == 3);
    REQUIRE(rt.lookup(addr(PAGE * 100)) == 0);
}
