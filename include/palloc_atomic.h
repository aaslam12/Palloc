#pragma once

#include <cstddef>

#if defined(PALLOC_SINGLE_THREADED)

// Drop-in replacement for std::atomic<T> that compiles to plain loads/stores
// when the library is built in single-threaded mode. Eliminates LOCK-prefixed
// instructions (LOCK XADD, LOCK CMPXCHG, etc.) that cost ~15-20 cycles each.
#include <atomic> // only for std::memory_order (enum, zero cost)

namespace AL
{
template<typename T>
struct palloc_atomic
{
    T value;

    palloc_atomic() noexcept = default;
    constexpr palloc_atomic(T v) noexcept : value(v) {}

    T load([[maybe_unused]] std::memory_order order = std::memory_order_seq_cst) const noexcept
    {
        return value;
    }

    void store(T v, [[maybe_unused]] std::memory_order order = std::memory_order_seq_cst) noexcept
    {
        value = v;
    }

    T fetch_add(T v, [[maybe_unused]] std::memory_order order = std::memory_order_seq_cst) noexcept
    {
        T old = value;
        value += v;
        return old;
    }

    T fetch_sub(T v, [[maybe_unused]] std::memory_order order = std::memory_order_seq_cst) noexcept
    {
        T old = value;
        value -= v;
        return old;
    }

    palloc_atomic& operator=(T v) noexcept
    {
        value = v;
        return *this;
    }

    // prevent copy/move (mirrors std::atomic)
    palloc_atomic(const palloc_atomic&) = delete;
    palloc_atomic& operator=(const palloc_atomic&) = delete;
};
} // namespace AL

#else

#include <atomic>

namespace AL
{
template<typename T>
using palloc_atomic = std::atomic<T>;
} // namespace AL

#endif
