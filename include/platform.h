#pragma once

#include <cstddef>

#ifdef _WIN32
#include <windows.h>
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

namespace AL
{

//
// replaces platform specific system calls with a wrapper that changes which function is called based on what system you compiled for.
// has zero runtime overhead
//
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
