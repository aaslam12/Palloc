#include "bitmap.h"
#include <catch2/catch_test_macros.hpp>
#include <unordered_set>
#include <vector>

// ─── helpers ─────────────────────────────────────────────────────────────────

static AL::bitmap make(std::vector<uint8_t>& buf, size_t num_slots)
{
    buf.assign(AL::bitmap::required_size(num_slots), 0);
    AL::bitmap bm;
    bm.init(buf.data(), num_slots);
    return bm;
}

// ─── init ────────────────────────────────────────────────────────────────────

TEST_CASE("bitmap init", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 100);

    CHECK(bm.is_init());
    CHECK(bm.num_slots() == 100);
    CHECK(bm.num_words() == 2);
    CHECK(bm.free_count() == 100);
}

TEST_CASE("bitmap required_size rounds up to word boundary", "[bitmap]")
{
    CHECK(AL::bitmap::required_size(1)   == 8);
    CHECK(AL::bitmap::required_size(64)  == 8);
    CHECK(AL::bitmap::required_size(65)  == 16);
    CHECK(AL::bitmap::required_size(128) == 16);
    CHECK(AL::bitmap::required_size(129) == 24);
}

// ─── alloc_bit ───────────────────────────────────────────────────────────────

TEST_CASE("alloc_bit returns sequential slots on empty bitmap", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 64);

    for (size_t i = 0; i < 64; ++i)
        CHECK(bm.alloc_bit() == i);

    CHECK(bm.alloc_bit() == static_cast<size_t>(-1));
    CHECK(bm.free_count() == 0);
}

TEST_CASE("alloc_bit returns -1 when full", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 10);
    for (size_t i = 0; i < 10; ++i) bm.alloc_bit();
    CHECK(bm.alloc_bit() == static_cast<size_t>(-1));
}

TEST_CASE("alloc_bit no duplicates across word boundary", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 65); // 2 words, 1 valid slot in last word

    std::unordered_set<size_t> seen;
    for (size_t i = 0; i < 65; ++i)
    {
        size_t s = bm.alloc_bit();
        REQUIRE(s < 65);
        CHECK(seen.insert(s).second);
    }
    CHECK(bm.alloc_bit() == static_cast<size_t>(-1));
}

TEST_CASE("alloc_bit with limit", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 128);

    for (size_t i = 0; i < 10; ++i)
        CHECK(bm.alloc_bit(10) < 10);

    CHECK(bm.alloc_bit(10) == static_cast<size_t>(-1));
    CHECK(bm.alloc_bit(11) == 10); // slot 10 still free
}

TEST_CASE("alloc_bit limit=0 returns -1", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 64);
    CHECK(bm.alloc_bit(0) == static_cast<size_t>(-1));
}

TEST_CASE("alloc_bit limit larger than num_slots clamps correctly", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 10);
    for (size_t i = 0; i < 10; ++i) bm.alloc_bit(9999);
    CHECK(bm.alloc_bit(9999) == static_cast<size_t>(-1));
    CHECK(bm.free_count() == 0);
}

TEST_CASE("alloc_bit limit at exact word boundary", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 128);

    for (size_t i = 0; i < 64; ++i)
        CHECK(bm.alloc_bit(64) < 64);

    CHECK(bm.alloc_bit(64) == static_cast<size_t>(-1));
    CHECK(bm.alloc_bit(65) == 64); // first slot of second word
}

// ─── free_bit ────────────────────────────────────────────────────────────────

TEST_CASE("free_bit restores slot for reuse", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 64);
    for (size_t i = 0; i < 64; ++i) bm.alloc_bit();

    bm.free_bit(30);
    CHECK(bm.free_count() == 1);
    CHECK(bm.alloc_bit() == 30);
    CHECK(bm.free_count() == 0);
}

TEST_CASE("free_bit updates free_count", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 64);
    bm.alloc_bit();
    bm.alloc_bit();
    CHECK(bm.free_count() == 62);
    bm.free_bit(0);
    CHECK(bm.free_count() == 63);
}

// ─── alloc_bits_batch ────────────────────────────────────────────────────────

TEST_CASE("alloc_bits_batch returns unique slots", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 200);

    size_t out[200];
    size_t n = bm.alloc_bits_batch(150, out);
    REQUIRE(n == 150);

    std::unordered_set<size_t> seen(out, out + n);
    CHECK(seen.size() == 150);
    for (size_t s : seen) CHECK(s < 200);
    CHECK(bm.free_count() == 50);
}

TEST_CASE("alloc_bits_batch count=0 returns 0", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 64);
    size_t out[1];
    CHECK(bm.alloc_bits_batch(0, out) == 0);
}

TEST_CASE("alloc_bits_batch clamps to available slots", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 10);
    size_t out[20];
    CHECK(bm.alloc_bits_batch(20, out) == 10);
    CHECK(bm.free_count() == 0);
}

TEST_CASE("alloc_bits_batch with limit", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 128);

    size_t out[128];
    size_t n = bm.alloc_bits_batch(10, 32, out);
    REQUIRE(n == 10);
    for (size_t i = 0; i < n; ++i) CHECK(out[i] < 32);

    // drain remaining within limit
    n = bm.alloc_bits_batch(100, 32, out);
    REQUIRE(n == 22);
    for (size_t i = 0; i < n; ++i) CHECK(out[i] < 32);

    CHECK(bm.alloc_bits_batch(1, 32, out) == 0);
    CHECK(bm.free_count() == 96);
}

TEST_CASE("alloc_bits_batch limit=0 returns 0", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 64);
    size_t out[1];
    CHECK(bm.alloc_bits_batch(10, 0, out) == 0);
}

TEST_CASE("alloc_bits_batch with limit at exact word boundary", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 128);
    size_t out[64];
    size_t n = bm.alloc_bits_batch(64, 64, out);
    REQUIRE(n == 64);
    for (size_t i = 0; i < n; ++i) CHECK(out[i] < 64);
    CHECK(bm.alloc_bits_batch(1, 64, out) == 0);
}

// ─── free_bits_batch ─────────────────────────────────────────────────────────

TEST_CASE("free_bits_batch restores all slots", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 128);

    size_t slots[128];
    REQUIRE(bm.alloc_bits_batch(128, slots) == 128);
    CHECK(bm.free_count() == 0);

    bm.free_bits_batch(slots, 128);
    CHECK(bm.free_count() == 128);
    CHECK(bm.alloc_bits_batch(128, slots) == 128);
}

TEST_CASE("free_bits_batch accumulates per word (same word, sorted)", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 64);
    bm.alloc_bit(); bm.alloc_bit(); bm.alloc_bit(); // claim 0,1,2

    size_t slots[] = {0, 1, 2};
    bm.free_bits_batch(slots, 3);
    CHECK(bm.free_count() == 64);
    CHECK(!bm.is_slot_set(0));
    CHECK(!bm.is_slot_set(1));
    CHECK(!bm.is_slot_set(2));
}

TEST_CASE("free_bits_batch across word boundary", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 128);
    size_t slots[128];
    bm.alloc_bits_batch(128, slots);

    // free slots spanning two words
    size_t cross[] = {63, 64};
    bm.free_bits_batch(cross, 2);
    CHECK(bm.free_count() == 2);
    CHECK(!bm.is_slot_set(63));
    CHECK(!bm.is_slot_set(64));
}

// ─── is_slot_set / set_slot / clear_slot ─────────────────────────────────────

TEST_CASE("set_slot / clear_slot / is_slot_set", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 128);

    CHECK(!bm.is_slot_set(0));
    bm.set_slot(0);
    CHECK(bm.is_slot_set(0));
    bm.clear_slot(0);
    CHECK(!bm.is_slot_set(0));

    bm.set_slot(63);
    bm.set_slot(64);
    CHECK(bm.is_slot_set(63));
    CHECK(bm.is_slot_set(64));
    bm.clear_slot(63);
    CHECK(!bm.is_slot_set(63));
    CHECK(bm.is_slot_set(64));
}

// ─── find_lowest_set_bit ─────────────────────────────────────────────────────

TEST_CASE("find_lowest_set_bit empty returns -1", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 100);
    CHECK(bm.find_lowest_set_bit() == static_cast<size_t>(-1));
}

TEST_CASE("find_lowest_set_bit tracks minimum", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 200);

    bm.set_slot(99);
    CHECK(bm.find_lowest_set_bit() == 99);
    bm.set_slot(50);
    CHECK(bm.find_lowest_set_bit() == 50);
    bm.set_slot(0);
    CHECK(bm.find_lowest_set_bit() == 0);
}

TEST_CASE("find_lowest_set_bit at word boundary", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 128);

    bm.set_slot(64);
    CHECK(bm.find_lowest_set_bit() == 64);
    bm.set_slot(63);
    CHECK(bm.find_lowest_set_bit() == 63);
}

// ─── find_highest_set_bit ────────────────────────────────────────────────────

TEST_CASE("find_highest_set_bit empty returns -1", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 100);
    CHECK(bm.find_highest_set_bit() == static_cast<size_t>(-1));
}

TEST_CASE("find_highest_set_bit tracks maximum", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 200);

    bm.set_slot(50);
    CHECK(bm.find_highest_set_bit() == 50);
    bm.set_slot(99);
    CHECK(bm.find_highest_set_bit() == 99);
    bm.set_slot(199);
    CHECK(bm.find_highest_set_bit() == 199);
}

TEST_CASE("find_highest_set_bit ignores pre-set tail bits", "[bitmap]")
{
    // 65 slots: last word has 1 valid bit (64), bits 65-127 pre-set to 1 by init
    std::vector<uint8_t> buf;
    auto bm = make(buf, 65);

    CHECK(bm.find_highest_set_bit() == static_cast<size_t>(-1));

    bm.set_slot(64);
    CHECK(bm.find_highest_set_bit() == 64);

    bm.set_slot(0);
    CHECK(bm.find_highest_set_bit() == 64);

    bm.clear_slot(64);
    CHECK(bm.find_highest_set_bit() == 0);
}

TEST_CASE("find_highest_set_bit at word boundary", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 128);

    bm.set_slot(63);
    CHECK(bm.find_highest_set_bit() == 63);
    bm.set_slot(64);
    CHECK(bm.find_highest_set_bit() == 64);
    bm.clear_slot(64);
    CHECK(bm.find_highest_set_bit() == 63);
}

// ─── is_range_empty ──────────────────────────────────────────────────────────

TEST_CASE("is_range_empty", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 192); // 3 words

    CHECK(bm.is_range_empty(0, 3));

    bm.set_slot(65); // word 1
    CHECK(bm.is_range_empty(0, 1));
    CHECK(!bm.is_range_empty(1, 1));
    CHECK(!bm.is_range_empty(0, 3));

    bm.clear_slot(65);
    CHECK(bm.is_range_empty(0, 3));
}

// ─── reset ───────────────────────────────────────────────────────────────────

TEST_CASE("reset clears all slots and restores free_count", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 100);

    size_t out[100];
    bm.alloc_bits_batch(100, out);
    CHECK(bm.free_count() == 0);

    bm.reset();
    CHECK(bm.free_count() == 100);
    CHECK(bm.find_highest_set_bit() == static_cast<size_t>(-1));
    CHECK(bm.alloc_bits_batch(100, out) == 100);
}

TEST_CASE("reset_to clears only active portion", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 128);

    size_t out[128];
    bm.alloc_bits_batch(128, out);

    bm.reset_to(1, 64); // reset only first word
    CHECK(bm.free_count() == 64);

    size_t s = bm.alloc_bit();
    CHECK(s < 64);
}

// ─── free_count helpers ──────────────────────────────────────────────────────

TEST_CASE("set_free_count and fetch_add_free_count", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 64);

    bm.set_free_count(10);
    CHECK(bm.free_count() == 10);
    bm.fetch_add_free_count(5);
    CHECK(bm.free_count() == 15);
}

// ─── round-trip integrity ────────────────────────────────────────────────────

TEST_CASE("alloc/free round-trip across 5 words preserves uniqueness", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 320);

    std::vector<size_t> slots;
    slots.reserve(320);
    for (size_t i = 0; i < 320; ++i)
    {
        size_t s = bm.alloc_bit();
        REQUIRE(s < 320);
        slots.push_back(s);
    }

    std::unordered_set<size_t> unique(slots.begin(), slots.end());
    CHECK(unique.size() == 320);
    CHECK(bm.free_count() == 0);

    for (size_t s : slots) bm.free_bit(s);
    CHECK(bm.free_count() == 320);
    CHECK(bm.find_lowest_set_bit() == static_cast<size_t>(-1));
}

TEST_CASE("interleaved alloc and free", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 64);

    size_t a = bm.alloc_bit();
    size_t b = bm.alloc_bit();
    size_t c = bm.alloc_bit();
    bm.free_bit(b);
    size_t d = bm.alloc_bit(); // should reclaim b
    CHECK(d == b);
    bm.free_bit(a);
    bm.free_bit(c);
    bm.free_bit(d);
    CHECK(bm.free_count() == 64);
}

TEST_CASE("batch alloc then batch free full cycle", "[bitmap]")
{
    std::vector<uint8_t> buf;
    auto bm = make(buf, 256);

    size_t slots[256];
    REQUIRE(bm.alloc_bits_batch(256, slots) == 256);
    CHECK(bm.free_count() == 0);

    // sort so free_bits_batch can accumulate per word
    std::sort(slots, slots + 256);
    bm.free_bits_batch(slots, 256);
    CHECK(bm.free_count() == 256);

    REQUIRE(bm.alloc_bits_batch(256, slots) == 256);
    CHECK(bm.free_count() == 0);
}
