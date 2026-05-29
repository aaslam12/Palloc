#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// Low-Overhead Benchmark Utilities
//
// Optimizations:
//   - RDTSC direct cycle counts (single instruction, no function call)
//   - Fixed-size preallocated latency buffers (no heap allocation)
//   - Per-thread counters with single atomic aggregation at end
//   - Histogram-based percentile estimation (no sorting)
//   - Deadline via separate thread (no clock reads in hot path)
// ═══════════════════════════════════════════════════════════════════════════════

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <thread>

namespace bench
{

// ─── RDTSC Cycle Counter ──────────────────────────────────────────────────────

inline uint64_t rdtsc()
{
    uint64_t a, d;
    asm volatile("rdtsc" : "=a"(a), "=d"(d));
    return (d << 32) | a;
}

// ─── Compiler Hints ───────────────────────────────────────────────────────────

inline void escape(void* p)
{
    asm volatile("" : : "g"(p) : "memory");
}
inline void clobber()
{
    asm volatile("" : : : "memory");
}

// ─── Fixed-Size Latency Buffer ────────────────────────────────────────────────

template<size_t Capacity>
struct LatencyBuffer
{
    alignas(64) uint64_t samples[Capacity];
    size_t idx = 0;

    void record(uint64_t cycles)
    {
        if (idx < Capacity)
            samples[idx++] = cycles;
    }

    struct Stats
    {
        uint64_t p50 = 0, p90 = 0, p99 = 0, p999 = 0;
        double mean = 0;
    };

    Stats compute() const
    {
        if (idx == 0)
            return {};

        // Copy to sort (avoid modifying original)
        uint64_t* sorted = const_cast<uint64_t*>(samples);
        std::sort(sorted, sorted + idx);

        Stats s;
        s.p50 = sorted[idx * 50 / 100];
        s.p90 = sorted[idx * 90 / 100];
        s.p99 = sorted[idx * 99 / 100];
        s.p999 = sorted[idx * 999 / 1000];
        double sum = std::accumulate(sorted, sorted + idx, 0.0);
        s.mean = sum / static_cast<double>(idx);
        return s;
    }
};

// ─── Histogram for Approximate Percentiles (no sorting) ───────────────────────

struct LatencyHistogram
{
    // 64 buckets, log2 scale (0-63 bits)
    static constexpr size_t NUM_BUCKETS = 64;
    alignas(64) std::atomic<uint64_t> buckets[NUM_BUCKETS]{};

    void record(uint64_t cycles)
    {
        // Find highest set bit position = bucket index
        size_t bucket = (cycles == 0) ? 0 : (63 - __builtin_clzll(cycles));
        if (bucket >= NUM_BUCKETS)
            bucket = NUM_BUCKETS - 1;
        buckets[bucket].fetch_add(1, std::memory_order_relaxed);
    }

    // Estimate percentile from histogram
    // Returns the bucket boundary as an approximation
    uint64_t percentile(double p) const
    {
        uint64_t total = 0;
        for (size_t i = 0; i < NUM_BUCKETS; ++i)
            total += buckets[i].load(std::memory_order_relaxed);

        if (total == 0)
            return 0;

        uint64_t target = static_cast<uint64_t>(total * p / 100.0);
        uint64_t cumulative = 0;

        for (size_t i = 0; i < NUM_BUCKETS; ++i)
        {
            cumulative += buckets[i].load(std::memory_order_relaxed);
            if (cumulative >= target)
                return static_cast<uint64_t>(1ULL << i); // bucket lower bound
        }

        return static_cast<uint64_t>(1ULL << (NUM_BUCKETS - 1));
    }
};

// ─── Deadline Timer (separate thread, no hot-path overhead) ───────────────────

class DeadlineTimer
{
    std::atomic<bool> done_{false};
    std::thread thread_;

public:
    explicit DeadlineTimer(int seconds) : done_(false)
    {
        thread_ = std::thread([this, seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            done_.store(true, std::memory_order_relaxed);
        });
    }

    ~DeadlineTimer()
    {
        if (thread_.joinable())
            thread_.join();
    }

    bool is_done() const
    {
        return done_.load(std::memory_order_relaxed);
    }

    // For periodic checking (returns true every N calls when deadline is reached)
    template<size_t CheckInterval>
    bool check_periodic(size_t iteration) const
    {
        if ((iteration & (CheckInterval - 1)) == 0)
            return is_done();
        return false;
    }
};

// ─── Per-Thread Counter ───────────────────────────────────────────────────────

template<typename T = size_t>
class ThreadCounter
{
    static inline thread_local T local_{0};
    std::atomic<T> global_{0};

public:
    void increment(T delta = 1)
    {
        local_ += delta;
    }

    T get_local() const
    {
        return local_;
    }

    void publish()
    {
        global_.fetch_add(local_, std::memory_order_relaxed);
        local_ = 0;
    }

    T get_global() const
    {
        return global_.load(std::memory_order_relaxed);
    }
};

// ─── Thread Coordination ──────────────────────────────────────────────────────

inline void wait_for_start(const std::atomic<bool>& start)
{
    while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
}

inline size_t worker_count()
{
    const unsigned hw = std::thread::hardware_concurrency();
    return (hw == 0) ? 8 : std::min<size_t>(hw, 16);
}

// ─── Timing Helpers ───────────────────────────────────────────────────────────

inline double cycles_per_op(uint64_t cycles, size_t ops)
{
    return static_cast<double>(cycles) / static_cast<double>(ops);
}

inline double mops_per_s(double elapsed_sec, size_t ops)
{
    return static_cast<double>(ops) / elapsed_sec / 1e6;
}

// ─── Print Helpers ────────────────────────────────────────────────────────────

inline void print_table_header(const char* title = nullptr)
{
    if (title)
        std::printf("\n━━━ %s ━━━\n\n", title);
    std::printf("  %-22s %10s %12s\n", "Allocator", "ns/op", "MOps/s");
    std::printf("  ──────────────────────────────────────────────\n");
}

inline void print_row(const char* name, double ns, double mops)
{
    std::printf("  %-22s %8.1f %12.1f\n", name, ns, mops);
}

} // namespace bench
