#include "arena.h"
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring> // for std::memset
#include <unistd.h>

const size_t PAGE_SIZE = getpagesize();

static void check_arena_valid(const AL::arena& a)
{
    REQUIRE(a.get_used() == 0);
    REQUIRE(a.get_capacity() > 0);
}

TEST_CASE("Arena creation", "[arena]")
{
    AL::arena a(PAGE_SIZE);
    check_arena_valid(a);
}

TEST_CASE("Arena allocation", "[arena]")
{
    AL::arena a(PAGE_SIZE);
    check_arena_valid(a);

    size_t* num = static_cast<size_t*>(a.alloc(sizeof(size_t)));
    REQUIRE(num != nullptr);
}

TEST_CASE("Arena alloc beyond capacity", "[arena]")
{
    AL::arena a(PAGE_SIZE);
    check_arena_valid(a);

    // the 2 is just an arbitrary number.
    // we just need to allocate more than the capacity
    size_t* num = static_cast<size_t*>(a.alloc(a.get_capacity() * 2));
    REQUIRE(num == nullptr);
}

TEST_CASE("Arena reset", "[arena]")
{
    AL::arena a(PAGE_SIZE);
    check_arena_valid(a);

    size_t* num = static_cast<size_t*>(a.alloc(sizeof(size_t)));
    REQUIRE(num != nullptr);

    REQUIRE(a.get_used() >= sizeof(size_t));

    a.reset();

    check_arena_valid(a);
}

TEST_CASE("Arena zero allocation", "[arena]")
{
    AL::arena a(PAGE_SIZE);

    void* ptr = a.alloc(0);
    REQUIRE(ptr == nullptr);    // Should return null for 0 bytes
    REQUIRE(a.get_used() == 0); // Should not change used count
}

TEST_CASE("Arena sequential allocations", "[arena]")
{
    AL::arena a(PAGE_SIZE);

    void* p1 = a.alloc(64);
    void* p2 = a.alloc(64);

    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    REQUIRE(p1 != p2);

    // Check they don't overlap (p2 should be after p1)
    char* c1 = static_cast<char*>(p1);
    char* c2 = static_cast<char*>(p2);
    REQUIRE(c2 >= c1 + 64);
}

TEST_CASE("Arena calloc zeros memory", "[arena]")
{
    AL::arena a(PAGE_SIZE);

    // Allocate and dirty some memory first
    char* dirty = static_cast<char*>(a.alloc(64));
    std::memset(dirty, 0xFF, 64); // Fill with garbage

    a.reset();

    // Now calloc should return zeroed memory
    char* clean = static_cast<char*>(a.calloc(64));
    REQUIRE(clean != nullptr);

    for (size_t i = 0; i < 64; ++i)
    {
        REQUIRE(clean[i] == 0);
    }
}

TEST_CASE("Arena exact capacity allocation", "[arena]")
{
    AL::arena a(PAGE_SIZE);

    // Allocate exactly the full capacity
    void* ptr = a.alloc(a.get_capacity());
    REQUIRE(ptr != nullptr);
    REQUIRE(a.get_used() == a.get_capacity());

    // Next allocation should fail
    void* ptr2 = a.alloc(1);
    REQUIRE(ptr2 == nullptr);
}
