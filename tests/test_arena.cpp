#include "arena.h"
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>

static void check_arena_valid(const arena& a)
{
    REQUIRE(a.get_used() == 0);
    REQUIRE(a.get_capacity() > 0);
}

TEST_CASE("Arena creation", "[arena]")
{
    auto a = std::make_unique<arena>();
    check_arena_valid(*a);
}

TEST_CASE("Arena allocation", "[arena]")
{
    auto a = std::make_unique<arena>();
    check_arena_valid(*a);

    size_t* num = static_cast<size_t*>(a->alloc(sizeof(size_t)));
    REQUIRE(num != nullptr);
}

TEST_CASE("Arena alloc beyond capacity", "[arena]")
{
    auto a = std::make_unique<arena>();
    check_arena_valid(*a);

    // the 2 is just an arbitrary number.
    // we just need to allocate more than the capacity
    size_t* num = static_cast<size_t*>(a->alloc(a->get_capacity() * 2));
    REQUIRE(num == nullptr);
}

TEST_CASE("Arena reset", "[arena]")
{
    auto a = std::make_unique<arena>();
    check_arena_valid(*a);

    size_t* num = static_cast<size_t*>(a->alloc(sizeof(size_t)));
    REQUIRE(num != nullptr);

    REQUIRE(a->get_used() >= sizeof(size_t));

    a->reset();

    check_arena_valid(*a);
}
