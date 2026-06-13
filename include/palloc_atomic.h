#pragma once

#include <atomic>

// PALLOC_SINGLE_THREADED_OVERRIDE: force all allocator components to non-threaded mode.
// Overrides the per-component Tthreaded template parameter default.
#if defined(PALLOC_SINGLE_THREADED_OVERRIDE) || defined(PALLOC_SINGLE_THREADED)
inline constexpr bool PALLOC_THREADED_DEFAULT = false;
#else
inline constexpr bool PALLOC_THREADED_DEFAULT = true;
#endif

namespace AL
{

// plain-value wrapper with the same interface as std::atomic.
// used when Tthreaded=false — compiles to plain loads/stores, no LOCK prefix.
template<typename T>
struct plain_atomic
{
    T value;

    plain_atomic() noexcept = default;
    constexpr plain_atomic(T v) noexcept : value(v)
    {}

    T load([[maybe_unused]] std::memory_order = std::memory_order_seq_cst) const noexcept
    {
        return value;
    }
    void store(T v, [[maybe_unused]] std::memory_order = std::memory_order_seq_cst) noexcept
    {
        value = v;
    }

    T fetch_add(T v, [[maybe_unused]] std::memory_order = std::memory_order_seq_cst) noexcept
    {
        T old = value;
        value += v;
        return old;
    }
    T fetch_sub(T v, [[maybe_unused]] std::memory_order = std::memory_order_seq_cst) noexcept
    {
        T old = value;
        value -= v;
        return old;
    }
    T fetch_and(T v, [[maybe_unused]] std::memory_order = std::memory_order_seq_cst) noexcept
    {
        T old = value;
        value &= v;
        return old;
    }
    T fetch_or(T v, [[maybe_unused]] std::memory_order = std::memory_order_seq_cst) noexcept
    {
        T old = value;
        value |= v;
        return old;
    }
    T fetch_xor(T v, [[maybe_unused]] std::memory_order = std::memory_order_seq_cst) noexcept
    {
        T old = value;
        value ^= v;
        return old;
    }
    T exchange(T v, [[maybe_unused]] std::memory_order = std::memory_order_seq_cst) noexcept
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

    plain_atomic& operator=(T v) noexcept
    {
        value = v;
        return *this;
    }
    plain_atomic(const plain_atomic&) = delete;
    plain_atomic& operator=(const plain_atomic&) = delete;
};

// palloc_atomic<T, Tthreaded>:
//   Tthreaded=true  -> std::atomic<T>   (real atomics, LOCK-prefixed instructions)
//   Tthreaded=false -> plain_atomic<T>  (plain loads/stores, zero overhead)
template<typename T, bool Tthreaded = PALLOC_THREADED_DEFAULT>
using palloc_atomic = std::conditional_t<Tthreaded, std::atomic<T>, plain_atomic<T>>;

} // namespace AL
