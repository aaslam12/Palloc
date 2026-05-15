#include "slab.h"
#include <catch2/catch_test_macros.hpp>

#if defined(__linux__)

#include <cstddef>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
struct mem_status_kb
{
    std::uint64_t vm_size = 0;
    std::uint64_t vm_rss = 0;
    std::uint64_t rss_anon = 0;
    std::uint64_t vm_swap = 0;
};

struct largest_mapping_kb
{
    std::uint64_t size = 0;
    std::uint64_t rss = 0;
};

std::uint64_t parse_kb_value(const std::string& line)
{
    std::istringstream iss(line);
    std::string key;
    std::uint64_t value = 0;
    std::string unit;
    iss >> key >> value >> unit;
    return value;
}

mem_status_kb read_status_kb()
{
    std::ifstream in("/proc/self/status");
    REQUIRE(in.good());

    mem_status_kb s{};
    std::string line;
    while (std::getline(in, line))
    {
        if (line.rfind("VmSize:", 0) == 0)
            s.vm_size = parse_kb_value(line);
        else if (line.rfind("VmRSS:", 0) == 0)
            s.vm_rss = parse_kb_value(line);
        else if (line.rfind("RssAnon:", 0) == 0)
            s.rss_anon = parse_kb_value(line);
        else if (line.rfind("VmSwap:", 0) == 0)
            s.vm_swap = parse_kb_value(line);
    }
    return s;
}

largest_mapping_kb read_largest_mapping_kb()
{
    std::ifstream in("/proc/self/smaps");
    REQUIRE(in.good());

    largest_mapping_kb largest{};
    std::uint64_t current_size = 0;
    std::uint64_t current_rss = 0;
    bool have_entry = false;

    auto commit_entry = [&] {
        if (!have_entry)
            return;
        if (current_size > largest.size)
        {
            largest.size = current_size;
            largest.rss = current_rss;
        }
    };

    std::string line;
    while (std::getline(in, line))
    {
        // mapping header lines begin with hex addresses: "start-end perms ..."
        if (!line.empty() && std::isxdigit(static_cast<unsigned char>(line[0])) != 0)
        {
            commit_entry();
            current_size = 0;
            current_rss = 0;
            have_entry = true;
            continue;
        }

        if (line.rfind("Size:", 0) == 0)
            current_size = parse_kb_value(line);
        else if (line.rfind("Rss:", 0) == 0)
            current_rss = parse_kb_value(line);
    }

    commit_entry();
    return largest;
}
} // namespace

TEST_CASE("Slab: Linux reserve-first mapping inflates virtual size with low RSS", "[slab][linux][virtual]")
{
    constexpr std::uint64_t EXPECTED_RESERVE_KB = 104857216ULL; // 100 GiB
    constexpr std::uint64_t ONE_GIB_KB = 1024ULL * 1024ULL;

    const auto before = read_status_kb();
    AL::default_slab s{};
    const auto largest = read_largest_mapping_kb();
    const auto after = read_status_kb();

    const std::uint64_t vm_size_delta = (after.vm_size >= before.vm_size) ? (after.vm_size - before.vm_size) : 0;

    // expect a very large reserved mapping (~100 GiB) that stays non-resident
    REQUIRE(largest.size >= EXPECTED_RESERVE_KB - ONE_GIB_KB);
    REQUIRE(largest.rss == 0);

    // verify reserve-first behavior inflates virtual size by ~100 GiB
    REQUIRE(vm_size_delta >= EXPECTED_RESERVE_KB - ONE_GIB_KB);
    REQUIRE(vm_size_delta <= EXPECTED_RESERVE_KB + ONE_GIB_KB);

    // physical memory should stay low despite huge virtual reservation
    REQUIRE(after.vm_size >= EXPECTED_RESERVE_KB);
    REQUIRE(after.vm_rss <= 64ULL * 1024ULL);  // <= 64 MiB
    REQUIRE(after.rss_anon <= 16ULL * 1024ULL); // <= 16 MiB
    REQUIRE(after.vm_swap == 0);
}

#else

TEST_CASE("Slab: Linux reserve-first mapping inflates virtual size with low RSS", "[slab][linux][virtual]")
{
    SUCCEED("Linux-only test");
}

#endif
