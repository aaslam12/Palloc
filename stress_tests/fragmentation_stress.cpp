// ═══════════════════════════════════════════════════════════════════════════════
// Fragmentation Stress Test — Realistic Allocator Benchmark
//
// Simulates a long-running server workload where objects of mixed sizes
// are allocated and freed with varying lifetimes. Some objects are short-lived
// (freed within a few ops), others persist for thousands of operations.
//
// This creates realistic memory fragmentation: after many cycles, the heap
// contains scattered free regions that may be hard to coalesce. Measures:
//   - Throughput stability over time (does allocation slow down?)
//   - Latency distribution (p50/p99/p99.9)
//   - RSS vs live data (fragmentation ratio)
//
// Only allocators that handle variable-size individual free are tested:
//   Dynamic Slab, jemalloc, malloc
// ═══════════════════════════════════════════════════════════════════════════════

#include "dynamic_slab.h"

#include <jemalloc/jemalloc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

using namespace AL;
using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::nanoseconds;

inline void escape(void* p) { asm volatile("" : : "g"(p) : "memory"); }
inline void clobber() { asm volatile("" : : : "memory"); }

// ─── Test parameters ─────────────────────────────────────────────────────────

static constexpr int DURATION_SECS = 10;
static constexpr size_t NUM_SLOTS = 50'000;
static constexpr size_t LATENCY_CAPACITY = 2'000'000;

// Size classes that match realistic object sizes
static constexpr size_t SIZES[] = {16, 32, 64, 128, 256, 512};
static constexpr size_t NUM_SIZES = sizeof(SIZES) / sizeof(SIZES[0]);

// ─── RSS measurement ─────────────────────────────────────────────────────────

static size_t get_rss_bytes()
{
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    size_t virt = 0, rss = 0;
    if (fscanf(f, "%zu %zu", &virt, &rss) != 2)
        rss = 0;
    fclose(f);
    return rss * 4096;
}

// ─── Latency recorder ───────────────────────────────────────────────────────

struct LatencyRecorder
{
    std::vector<uint64_t> samples;
    size_t idx = 0;

    explicit LatencyRecorder(size_t cap) : samples(cap) {}

    void record(uint64_t ns)
    {
        if (idx < samples.size()) samples[idx++] = ns;
    }

    struct Stats
    {
        uint64_t p50 = 0, p90 = 0, p99 = 0, p999 = 0;
        double mean = 0;
    };

    Stats compute()
    {
        if (idx == 0) return {};
        std::sort(samples.begin(), samples.begin() + idx);
        Stats s;
        s.p50 = samples[idx * 50 / 100];
        s.p90 = samples[idx * 90 / 100];
        s.p99 = samples[idx * 99 / 100];
        s.p999 = samples[idx * 999 / 1000];
        double sum = std::accumulate(samples.begin(), samples.begin() + idx, 0.0);
        s.mean = sum / static_cast<double>(idx);
        return s;
    }
};

// ─── Slot-based workload ─────────────────────────────────────────────────────
// NUM_SLOTS slots, each holds a pointer + size. On each op, pick a random slot:
//   - If occupied: free it (varying lifetime)
//   - Allocate a new random-sized object into the slot
//   - Write data into it (exercise cache)
// This creates realistic fragmentation: objects of different sizes come and go,
// some slots get recycled quickly, others persist for thousands of ops.

struct Slot
{
    void* ptr = nullptr;
    size_t size = 0;
};

struct BenchResult
{
    const char* name;
    size_t ops;
    double elapsed_sec;
    size_t rss_start_mb;
    size_t rss_end_mb;
    size_t live_data_mb;
    LatencyRecorder::Stats latency;
};

template <typename AllocFn, typename FreeFn>
BenchResult run_fragmentation(const char* name, AllocFn alloc_fn, FreeFn free_fn)
{
    std::vector<Slot> slots(NUM_SLOTS);
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> slot_dist(0, NUM_SLOTS - 1);
    std::uniform_int_distribution<size_t> size_idx_dist(0, NUM_SIZES - 1);

    LatencyRecorder recorder(LATENCY_CAPACITY);
    size_t ops = 0;
    size_t live_bytes = 0;
    size_t live_count = 0;

    // Phase 1: Fill all slots to create baseline memory usage
    for (size_t i = 0; i < NUM_SLOTS; i++)
    {
        size_t sz = SIZES[size_idx_dist(rng)];
        void* mem = alloc_fn(sz);
        if (mem)
        {
            std::memset(mem, static_cast<int>(i & 0xFF), sz);
            escape(mem);
            slots[i] = {mem, sz};
            live_bytes += sz;
            live_count++;
        }
    }

    size_t rss_start = get_rss_bytes() / (1024 * 1024);

    // Phase 2: Churn — randomly replace slots for DURATION_SECS
    auto start = Clock::now();
    auto deadline = start + std::chrono::seconds(DURATION_SECS);

    while (Clock::now() < deadline)
    {
        bool sample = (ops & 127) == 0;
        auto t0 = sample ? Clock::now() : Clock::time_point{};

        size_t slot_idx = slot_dist(rng);
        Slot& slot = slots[slot_idx];

        // Free existing object
        if (slot.ptr)
        {
            // Read before freeing (simulate processing)
            volatile uint8_t v = *static_cast<uint8_t*>(slot.ptr);
            (void)v;
            free_fn(slot.ptr, slot.size);
            live_bytes -= slot.size;
            live_count--;
            slot.ptr = nullptr;
            slot.size = 0;
        }

        // Allocate new object of random size
        size_t new_sz = SIZES[size_idx_dist(rng)];
        void* mem = alloc_fn(new_sz);
        if (mem)
        {
            // Write realistic data pattern
            std::memset(mem, static_cast<int>(ops & 0xFF), new_sz);
            escape(mem);
            clobber();
            slot.ptr = mem;
            slot.size = new_sz;
            live_bytes += new_sz;
            live_count++;
        }

        if (sample)
        {
            auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - t0).count();
            recorder.record(static_cast<uint64_t>(elapsed));
        }
        ops++;
    }

    double total_elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    size_t rss_end = get_rss_bytes() / (1024 * 1024);

    // Cleanup
    for (auto& slot : slots)
    {
        if (slot.ptr)
        {
            free_fn(slot.ptr, slot.size);
            slot.ptr = nullptr;
        }
    }

    return {name, ops, total_elapsed, rss_start, rss_end, live_bytes / (1024 * 1024), recorder.compute()};
}

// ─── Print helpers ───────────────────────────────────────────────────────────

void print_results(const std::vector<BenchResult>& results)
{
    printf("\n  %-22s %10s %12s %10s %10s\n", "Allocator", "ns/op", "MOps/s", "RSS(MB)", "LiveMB");
    printf("  ───────────────────────────────────────────────────────────────────\n");
    for (const auto& r : results)
    {
        double ns = (r.elapsed_sec * 1e9) / static_cast<double>(r.ops);
        double mops = static_cast<double>(r.ops) / r.elapsed_sec / 1e6;
        printf("  %-22s %8.1f %12.1f %8zu %8zu\n",
               r.name, ns, mops, r.rss_end_mb, r.live_data_mb);
    }

    printf("\n  %-22s %8s %8s %8s %8s %8s\n", "Allocator", "p50", "p90", "p99", "p99.9", "mean");
    printf("  ──────────────────────────────────────────────────────────────\n");
    for (const auto& r : results)
    {
        printf("  %-22s %6lu %8lu %8lu %8lu %8.1f ns\n",
               r.name, r.latency.p50, r.latency.p90, r.latency.p99, r.latency.p999, r.latency.mean);
    }
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main()
{
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║     Fragmentation Stress — Realistic Allocator Benchmark   ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  %zu slots, mixed sizes (%zu-%zuB), random replacement        ║\n",
           NUM_SLOTS, SIZES[0], SIZES[NUM_SIZES - 1]);
    printf("║  Duration: %d seconds per allocator                        ║\n", DURATION_SECS);
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    std::vector<BenchResult> results;

    // Dynamic Slab
    {
        default_dynamic_slab ds{};
        results.push_back(run_fragmentation(
            "Dynamic Slab",
            [&](size_t sz) -> void* { return ds.palloc(sz); },
            [&](void* p, size_t sz) { ds.free(p, sz); }));
    }

    // jemalloc
    {
        results.push_back(run_fragmentation(
            "jemalloc",
            [](size_t sz) -> void* { return mallocx(sz, 0); },
            [](void* p, size_t) { dallocx(p, 0); }));
    }

    // glibc malloc
    {
        results.push_back(run_fragmentation(
            "malloc",
            [](size_t sz) -> void* { return std::malloc(sz); },
            [](void* p, size_t) { std::free(p); }));
    }

    printf("\n━━━ Fragmentation Stress (%zu slots, %ds each) ━━━\n", NUM_SLOTS, DURATION_SECS);
    print_results(results);

    return 0;
}
