// ═══════════════════════════════════════════════════════════════════════════════
// Producer-Consumer Simulation — Realistic Allocator Benchmark
//
// Simulates the SPSC (Single Producer, Single Consumer) pipeline pattern
// common in trading engines: one thread allocates messages, another processes
// and frees them. This exercises cross-thread allocation/deallocation, which
// is challenging for thread-local caching allocators.
//
// The producer allocates messages, writes sequence data, and enqueues them.
// The consumer dequeues, verifies content, and frees. A lock-free ring buffer
// connects the two threads.
//
// Allocators tested: Pool, Slab, Dynamic Slab, jemalloc, malloc
// Mode: Multi-threaded (1 producer + 1 consumer)
// ═══════════════════════════════════════════════════════════════════════════════

#include "dynamic_slab.h"
#include "pool.h"
#include "slab.h"

#include <jemalloc/jemalloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <thread>
#include <vector>

using namespace AL;
using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::nanoseconds;

inline void escape(void* p) { asm volatile("" : : "g"(p) : "memory"); }
inline void clobber() { asm volatile("" : : : "memory"); }

// ─── Test parameters ─────────────────────────────────────────────────────────

static constexpr int DURATION_SECS = 7;
static constexpr size_t QUEUE_SIZE = 8192; // power of 2
static constexpr size_t LATENCY_CAPACITY = 2'000'000;
static constexpr size_t POOL_CAPACITY = 200'000;

// ─── Message struct ──────────────────────────────────────────────────────────

struct Message
{
    uint64_t sequence;
    uint64_t produce_ts;
    uint64_t checksum;
    uint64_t payload[5]; // fills to 64 bytes total
};

static constexpr size_t MSG_SIZE = sizeof(Message);
static_assert(MSG_SIZE == 64, "Message should be 64 bytes");

// ─── Lock-free SPSC ring buffer ──────────────────────────────────────────────

struct SPSCQueue
{
    struct Entry
    {
        void* ptr;
    };

    alignas(64) std::array<Entry, QUEUE_SIZE> ring{};
    alignas(64) std::atomic<uint64_t> head{0}; // written by producer
    alignas(64) std::atomic<uint64_t> tail{0}; // written by consumer

    bool try_push(void* ptr)
    {
        uint64_t h = head.load(std::memory_order_relaxed);
        if (h - tail.load(std::memory_order_acquire) >= QUEUE_SIZE)
            return false;
        ring[h & (QUEUE_SIZE - 1)].ptr = ptr;
        head.store(h + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(void*& ptr)
    {
        uint64_t t = tail.load(std::memory_order_relaxed);
        if (t >= head.load(std::memory_order_acquire))
            return false;
        ptr = ring[t & (QUEUE_SIZE - 1)].ptr;
        tail.store(t + 1, std::memory_order_release);
        return true;
    }
};

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

// ─── Benchmark result ────────────────────────────────────────────────────────

struct BenchResult
{
    const char* name;
    size_t messages;
    double elapsed_sec;
    LatencyRecorder::Stats produce_latency;
    LatencyRecorder::Stats e2e_latency; // end-to-end: alloc → free
};

// ─── Test runner ─────────────────────────────────────────────────────────────

template <typename AllocFn, typename FreeFn>
BenchResult run_producer_consumer(const char* name, AllocFn alloc_fn, FreeFn free_fn)
{
    SPSCQueue queue;
    std::atomic<bool> producer_done{false};
    std::atomic<size_t> produced{0};
    std::atomic<size_t> consumed{0};

    LatencyRecorder produce_recorder(LATENCY_CAPACITY);
    LatencyRecorder e2e_recorder(LATENCY_CAPACITY);

    // Producer thread: allocate, write, enqueue
    std::thread producer([&] {
        uint64_t seq = 0;
        auto deadline = Clock::now() + std::chrono::seconds(DURATION_SECS);

        while (Clock::now() < deadline)
        {
            bool sample = (seq & 127) == 0;
            auto t0 = sample ? Clock::now() : Clock::time_point{};

            void* mem = alloc_fn();
            if (!mem)
            {
                // Backpressure: yield and retry
                std::this_thread::yield();
                continue;
            }

            auto* msg = static_cast<Message*>(mem);
            msg->sequence = seq;
            msg->produce_ts = static_cast<uint64_t>(
                std::chrono::duration_cast<Duration>(Clock::now().time_since_epoch()).count());
            // Write payload
            for (int i = 0; i < 5; i++)
                msg->payload[i] = seq * 7 + i;
            // Compute checksum over payload
            msg->checksum = 0;
            for (int i = 0; i < 5; i++)
                msg->checksum ^= msg->payload[i];
            escape(msg);

            // Enqueue with backpressure
            while (!queue.try_push(msg))
                std::this_thread::yield();

            if (sample)
            {
                auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - t0).count();
                produce_recorder.record(static_cast<uint64_t>(elapsed));
            }

            seq++;
            produced.fetch_add(1, std::memory_order_relaxed);
        }

        producer_done.store(true, std::memory_order_release);
    });

    // Consumer thread: dequeue, verify, free
    std::thread consumer([&] {
        while (true)
        {
            void* ptr = nullptr;
            if (queue.try_pop(ptr))
            {
                auto* msg = static_cast<Message*>(ptr);

                // Verify checksum (read payload)
                uint64_t check = 0;
                for (int i = 0; i < 5; i++)
                    check ^= msg->payload[i];
                escape(&check);

                // Measure end-to-end latency (sample every 128th)
                if ((msg->sequence & 127) == 0)
                {
                    auto now_ns = static_cast<uint64_t>(
                        std::chrono::duration_cast<Duration>(Clock::now().time_since_epoch()).count());
                    uint64_t e2e = now_ns - msg->produce_ts;
                    e2e_recorder.record(e2e);
                }

                free_fn(msg);
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
            else if (producer_done.load(std::memory_order_acquire))
            {
                // Drain remaining
                while (queue.try_pop(ptr))
                {
                    auto* msg = static_cast<Message*>(ptr);
                    uint64_t check = 0;
                    for (int i = 0; i < 5; i++)
                        check ^= msg->payload[i];
                    escape(&check);
                    free_fn(msg);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
            else
            {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    size_t total = consumed.load();

    // Compute elapsed from producer runtime
    double elapsed = static_cast<double>(DURATION_SECS);

    return {name, total, elapsed, produce_recorder.compute(), e2e_recorder.compute()};
}

// ─── Custom slab config for 64B messages ─────────────────────────────────────

constexpr std::array<size_class, 1> msg_slab_classes = {
    size_class{.byte_size = 64, .num_blocks = POOL_CAPACITY, .batch_size = 128}};
using msg_slab_cfg = slab_config<1, msg_slab_classes>;

// ─── Print helpers ───────────────────────────────────────────────────────────

void print_results(const std::vector<BenchResult>& results)
{
    printf("\n  %-22s %10s %12s %12s\n", "Allocator", "ns/msg", "MOps/s", "Messages");
    printf("  ───────────────────────────────────────────────────────────\n");
    for (const auto& r : results)
    {
        double ns = (r.elapsed_sec * 1e9) / static_cast<double>(r.messages);
        double mops = static_cast<double>(r.messages) / r.elapsed_sec / 1e6;
        printf("  %-22s %8.1f %12.1f %12zu\n", r.name, ns, mops, r.messages);
    }

    printf("\n  Producer latency (alloc + write + enqueue):\n");
    printf("  %-22s %8s %8s %8s %8s %8s\n", "Allocator", "p50", "p90", "p99", "p99.9", "mean");
    printf("  ──────────────────────────────────────────────────────────────\n");
    for (const auto& r : results)
    {
        printf("  %-22s %6lu %8lu %8lu %8lu %8.1f ns\n",
               r.name, r.produce_latency.p50, r.produce_latency.p90,
               r.produce_latency.p99, r.produce_latency.p999, r.produce_latency.mean);
    }

    printf("\n  End-to-end latency (alloc → verify → free):\n");
    printf("  %-22s %8s %8s %8s %8s %8s\n", "Allocator", "p50", "p90", "p99", "p99.9", "mean");
    printf("  ──────────────────────────────────────────────────────────────\n");
    for (const auto& r : results)
    {
        printf("  %-22s %6lu %8lu %8lu %8lu %8.1f ns\n",
               r.name, r.e2e_latency.p50, r.e2e_latency.p90,
               r.e2e_latency.p99, r.e2e_latency.p999, r.e2e_latency.mean);
    }
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main()
{
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║   Producer-Consumer Sim — Realistic Allocator Benchmark    ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  1 producer + 1 consumer thread, SPSC ring buffer (%zu)   ║\n", QUEUE_SIZE);
    printf("║  Message: %zu bytes, Duration: %d seconds per allocator     ║\n", MSG_SIZE, DURATION_SECS);
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    std::vector<BenchResult> results;

    // Pool
    {
        pool p(MSG_SIZE, POOL_CAPACITY);
        results.push_back(run_producer_consumer(
            "Pool",
            [&]() -> void* { return p.alloc(); },
            [&](Message* m) { p.free(m); }));
    }

    // Slab (custom config for 64B)
    {
        slab<msg_slab_cfg> s{};
        results.push_back(run_producer_consumer(
            "Slab (TLC)",
            [&]() -> void* { return s.alloc(MSG_SIZE); },
            [&](Message* m) { s.free(m, MSG_SIZE); }));
    }

    // Dynamic Slab
    {
        default_dynamic_slab ds{};
        results.push_back(run_producer_consumer(
            "Dynamic Slab",
            [&]() -> void* { return ds.palloc(MSG_SIZE); },
            [&](Message* m) { ds.free(m, MSG_SIZE); }));
    }

    // jemalloc
    {
        results.push_back(run_producer_consumer(
            "jemalloc",
            []() -> void* { return mallocx(MSG_SIZE, 0); },
            [](Message* m) { dallocx(m, 0); }));
    }

    // glibc malloc
    {
        results.push_back(run_producer_consumer(
            "malloc",
            []() -> void* { return std::malloc(MSG_SIZE); },
            [](Message* m) { std::free(m); }));
    }

    printf("\n━━━ Producer-Consumer Pipeline (SPSC, %ds each) ━━━\n", DURATION_SECS);
    print_results(results);

    return 0;
}
