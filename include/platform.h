#pragma once

#include <cstddef>
#include <cstring>

#ifdef _WIN32
#include <memoryapi.h>
#include <minwindef.h>
#include <windows.h>
#include <winnt.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

inline constexpr bool palloc_is_windows =
#ifdef _WIN32
    true;
#else
    false;
#endif

// portable compiler hints for cold/noinline functions
#if defined(__GNUC__) || defined(__clang__)
#define PALLOC_COLD __attribute__((noinline, cold))
#elif defined(_MSC_VER)
#define PALLOC_COLD __declspec(noinline)
#else
#define PALLOC_COLD
#endif

namespace AL
{

constexpr size_t ONE_KB = 1024;
constexpr size_t ONE_MB = 1024 * ONE_KB;
constexpr size_t ONE_GB = 1024 * ONE_MB;
constexpr size_t ONE_TB = 1024 * ONE_GB;

// platform-specific memory primitives - zero runtime overhead
struct platform_mem
{
    [[nodiscard]] static void* alloc(std::size_t size) noexcept
    {
#ifdef _WIN32
        return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return ptr == MAP_FAILED ? nullptr : ptr;
#endif
    }

    // reserves virtual address space without backing it with physical memory
    [[nodiscard]] static void* virtual_alloc(std::size_t size) noexcept
    {
#ifdef _WIN32
        return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
#else
        void* ptr = mmap(nullptr,
                         size,
                         PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                         -1,
                         0);
        return ptr == MAP_FAILED ? nullptr : ptr;
#endif
    }

    // commits reserved pages - makes them readable and writable
    static bool virtual_commit(void* ptr, std::size_t size) noexcept
    {
#ifdef _WIN32
        return VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE);
#else
        return mprotect(ptr, size, PROT_READ | PROT_WRITE) == 0;
#endif
    }

    // decommits physical pages and revokes access permissions
    static bool virtual_free(void* ptr, std::size_t size) noexcept
    {
#ifdef _WIN32
        return VirtualFree(ptr, size, MEM_DECOMMIT) != 0;
#else
        madvise(ptr, size, MADV_DONTNEED);
        return mprotect(ptr, size, PROT_NONE) == 0;
#endif
    }

    static bool free(void* ptr, std::size_t size) noexcept
    {
#ifdef _WIN32
        (void)size;
        return VirtualFree(ptr, 0, MEM_RELEASE) != 0;
#else
        return munmap(ptr, size) == 0;
#endif
    }

    static std::size_t page_size() noexcept
    {
#ifdef _WIN32
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return static_cast<std::size_t>(info.dwPageSize);
#else
        return static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
    }
};

} // namespace AL
