#pragma once

#include <atomic>

#if defined(PALLOC_SINGLE_THREADED)

// lightweight atomic wrapper - compiles to plain loads/stores under PALLOC_SINGLE_THREADED
// eliminates all LOCK-prefixed instructions

namespace AL
{
template<typename T>
struct palloc_atomic
{
    T value;

    palloc_atomic() noexcept = default;
    constexpr palloc_atomic(T v) noexcept : value(v)
    {}

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

    T fetch_and(T v, [[maybe_unused]] std::memory_order order = std::memory_order_seq_cst) noexcept
    {
        T old = value;
        value &= v;
        return old;
    }

    T fetch_or(T v, [[maybe_unused]] std::memory_order order = std::memory_order_seq_cst) noexcept
    {
        T old = value;
        value |= v;
        return old;
    }

    T fetch_xor(T v, [[maybe_unused]] std::memory_order order = std::memory_order_seq_cst) noexcept
    {
        T old = value;
        value ^= v;
        return old;
    }

    T exchange(T v, [[maybe_unused]] std::memory_order order = std::memory_order_seq_cst) noexcept
    {
        T old = value;
        value = v;
        return old;
    }

    bool compare_exchange_weak(T& expected,
                               T desired,
                               [[maybe_unused]] std::memory_order success = std::memory_order_seq_cst,
                               [[maybe_unused]] std::memory_order failure = std::memory_order_seq_cst) noexcept
    {
        if (value == expected)
        {
            value = desired;
            return true;
        }
        expected = value;
        return false;
    }

    bool compare_exchange_strong(T& expected,
                                 T desired,
                                 [[maybe_unused]] std::memory_order success = std::memory_order_seq_cst,
                                 [[maybe_unused]] std::memory_order failure = std::memory_order_seq_cst) noexcept
    {
        if (value == expected)
        {
            value = desired;
            return true;
        }
        expected = value;
        return false;
    }

    palloc_atomic& operator=(T v) noexcept
    {
        value = v;
        return *this;
    }

    palloc_atomic(const palloc_atomic&) = delete;
    palloc_atomic& operator=(const palloc_atomic&) = delete;
};
} // namespace AL

#else

namespace AL
{
template<typename T>
using palloc_atomic = std::atomic<T>;
} // namespace AL

#endif
