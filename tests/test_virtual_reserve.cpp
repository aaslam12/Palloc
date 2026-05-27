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
    // TSan adds significant shadow memory overhead, so allow more headroom under sanitizers
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    constexpr std::uint64_t MAX_ANON_KB = 64ULL * 1024ULL;  // 64 MiB under sanitizers
#else
    constexpr std::uint64_t MAX_ANON_KB = 20ULL * 1024ULL;  // 20 MiB: includes flat bitmaps (~400 KB) committed upfront
#endif
    REQUIRE(after.vm_size >= EXPECTED_RESERVE_KB);
    REQUIRE(after.vm_rss <= 64ULL * 1024ULL);  // <= 64 MiB
    REQUIRE(after.rss_anon <= MAX_ANON_KB);
    REQUIRE(after.vm_swap == 0);
}

#elif defined(_WIN32)

#include <cstdint>
#include <psapi.h>

namespace
{
struct process_mem_bytes
{
    std::uint64_t working_set = 0;
    std::uint64_t private_usage = 0;
    std::uint64_t pagefile_usage = 0;
};

struct vm_scan_bytes
{
    std::uint64_t total_mapped = 0;
    std::uint64_t largest_reserved = 0;
    std::uint64_t largest_commit = 0;
};

process_mem_bytes read_process_mem_bytes()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    const BOOL ok = GetProcessMemoryInfo(GetCurrentProcess(),
                                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                         sizeof(pmc));
    REQUIRE(ok != 0);

    process_mem_bytes out{};
    out.working_set = static_cast<std::uint64_t>(pmc.WorkingSetSize);
    out.private_usage = static_cast<std::uint64_t>(pmc.PrivateUsage);
    out.pagefile_usage = static_cast<std::uint64_t>(pmc.PagefileUsage);
    return out;
}

vm_scan_bytes scan_virtual_memory_bytes()
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    vm_scan_bytes out{};
    std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const std::uintptr_t max_addr = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < max_addr)
    {
        const SIZE_T q = VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi));
        if (q == 0)
            break;

        const std::uint64_t region_size = static_cast<std::uint64_t>(mbi.RegionSize);
        if (mbi.State != MEM_FREE)
            out.total_mapped += region_size;

        if (mbi.State == MEM_RESERVE && region_size > out.largest_reserved)
            out.largest_reserved = region_size;
        else if (mbi.State == MEM_COMMIT && region_size > out.largest_commit)
            out.largest_commit = region_size;

        const std::uintptr_t next = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= addr)
            break;
        addr = next;
    }

    return out;
}
} // namespace

TEST_CASE("Slab: Windows reserve-first mapping inflates virtual size with low RSS", "[slab][windows][virtual]")
{
    constexpr std::uint64_t EXPECTED_RESERVE_BYTES = AL::slab_config<>::VIRTUAL_MEM_PREALLOC_SIZE; // 100 GiB
    constexpr std::uint64_t ONE_GIB_BYTES = 1024ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t EXPECTED_COMMIT_BYTES = AL::slab_config<>::compute_total_region_size();

    const auto before_vm = scan_virtual_memory_bytes();
    const auto before_mem = read_process_mem_bytes();
    AL::default_slab s{};
    const auto after_vm = scan_virtual_memory_bytes();
    const auto after_mem = read_process_mem_bytes();

    const std::uint64_t mapped_delta =
        (after_vm.total_mapped >= before_vm.total_mapped) ? (after_vm.total_mapped - before_vm.total_mapped) : 0;
    const std::uint64_t working_set_delta =
        (after_mem.working_set >= before_mem.working_set) ? (after_mem.working_set - before_mem.working_set) : 0;
    const std::uint64_t private_usage_delta =
        (after_mem.private_usage >= before_mem.private_usage) ? (after_mem.private_usage - before_mem.private_usage) : 0;
    const std::uint64_t pagefile_delta =
        (after_mem.pagefile_usage >= before_mem.pagefile_usage) ? (after_mem.pagefile_usage - before_mem.pagefile_usage) : 0;

    // expect a very large reserved mapping (~100 GiB)
    REQUIRE(after_vm.largest_reserved >= EXPECTED_RESERVE_BYTES - ONE_GIB_BYTES);
    REQUIRE(after_vm.largest_reserved <= EXPECTED_RESERVE_BYTES + ONE_GIB_BYTES);

    // verify reserve-first behavior inflates virtual mappings by ~100 GiB
    REQUIRE(mapped_delta >= EXPECTED_RESERVE_BYTES - ONE_GIB_BYTES);
    REQUIRE(mapped_delta <= EXPECTED_RESERVE_BYTES + (2 * ONE_GIB_BYTES));

    // verify some committed rw region exists for pool metadata/payload
    REQUIRE(after_vm.largest_commit >= EXPECTED_COMMIT_BYTES);

    // physical/commit usage should stay low despite huge virtual reservation
    REQUIRE(working_set_delta <= 128ULL * 1024ULL * 1024ULL);  // <= 128 MiB
    REQUIRE(private_usage_delta <= 128ULL * 1024ULL * 1024ULL); // <= 128 MiB
    REQUIRE(pagefile_delta <= 256ULL * 1024ULL * 1024ULL);      // <= 256 MiB
}

#else

TEST_CASE("Slab: reserve-first mapping test", "[slab][virtual]")
{
    SUCCEED("platform-specific test");
}

#endif
