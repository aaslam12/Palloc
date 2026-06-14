// ═══════════════════════════════════════════════════════════════════════════════
// Producer-Consumer Simulation — Realistic Allocator Benchmark
//
// Simulates the SPSC pipeline pattern: one thread allocates messages,
// another processes and frees them. Exercises cross-thread allocation/deallocation.
//
// Allocators tested: Pool, Slab, jemalloc, malloc
// Mode: Multi-threaded (1 producer + 1 consumer)
// ═══════════════════════════════════════════════════════════════════════════════

#include <benchmark/benchmark.h>
#include "pool.h"
#include "slab.h"

#include <jemalloc/jemalloc.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace AL;

// ─── Test parameters ─────────────────────────────────────────────────────────

static constexpr size_t MSG_LIMIT     = 200'000;
static constexpr size_t QUEUE_SIZE = 65536;
static constexpr size_t POOL_CAPACITY = 200'000;

// ─── Message struct ───────────────────────────────────────────────────────────

struct Message
{
    uint64_t sequence;
    uint64_t produce_ts;
    uint64_t checksum;
    uint64_t payload[5];
};

static constexpr size_t MSG_SIZE = sizeof(Message);
static_assert(MSG_SIZE == 64, "Message should be 64 bytes");

// ─── Lock-free SPSC ring buffer ───────────────────────────────────────────────

struct SPSCQueue
{
    struct Entry { void* ptr; };

    alignas(64) std::array<Entry, QUEUE_SIZE> ring{};
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};

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

// ─── Benchmark result ─────────────────────────────────────────────────────────

struct BenchResult
{
    const char* name;
    size_t messages;
    double elapsed_sec;
};

// ─── Test runner ──────────────────────────────────────────────────────────────

template <typename AllocFn, typename FreeFn>
BenchResult run_producer_consumer(const char* name, AllocFn alloc_fn, FreeFn free_fn)
{
    SPSCQueue queue;
    std::atomic<bool> producer_done{false};
    size_t consumed = 0;

    std::thread producer([&] {
        uint64_t seq = 0;

        while (seq < MSG_LIMIT)
        {

            void* mem = alloc_fn();
            if (!mem) { std::this_thread::yield(); continue; }

            auto* msg = static_cast<Message*>(mem);
            msg->sequence = seq;
            msg->produce_ts = 0;
            for (int i = 0; i < 5; i++) msg->payload[i] = seq * 7 + i;
            msg->checksum = 0;
            for (int i = 0; i < 5; i++) msg->checksum ^= msg->payload[i];
            benchmark::DoNotOptimize(msg);

            while (!queue.try_push(msg)) std::this_thread::yield();
            seq++;
        }

        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        while (true)
        {
            void* ptr = nullptr;
            if (queue.try_pop(ptr))
            {
                auto* msg = static_cast<Message*>(ptr);
                uint64_t check = 0;
                for (int i = 0; i < 5; i++) check ^= msg->payload[i];
                benchmark::DoNotOptimize(check);
                free_fn(msg);
                ++consumed;
            }
            else if (producer_done.load(std::memory_order_acquire))
            {
                while (queue.try_pop(ptr))
                {
                    auto* msg = static_cast<Message*>(ptr);
                    uint64_t check = 0;
                    for (int i = 0; i < 5; i++) check ^= msg->payload[i];
                    benchmark::DoNotOptimize(check);
                    free_fn(msg);
                    ++consumed;
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

    return {name, consumed, 0.0};
}

// ─── Benchmarks ───────────────────────────────────────────────────────────────

static void BM_ProducerConsumer_Pool(benchmark::State& state)
{
    for (auto _ : state)
    {
        pool<> p(MSG_SIZE, POOL_CAPACITY);
        auto r = run_producer_consumer("Pool", [&]()->void*{return p.alloc();}, [&](Message*m){p.free(m);});
        benchmark::DoNotOptimize(r.messages);
    }
}
BENCHMARK(BM_ProducerConsumer_Pool)->Unit(benchmark::kMillisecond);

static void BM_ProducerConsumer_Slab(benchmark::State& state)
{
    for (auto _ : state)
    {
        default_slab s{};
        auto r = run_producer_consumer("Slab", [&]()->void*{return s.palloc(MSG_SIZE);}, [&](Message*m){s.free(m,MSG_SIZE);});
        benchmark::DoNotOptimize(r.messages);
    }
}
BENCHMARK(BM_ProducerConsumer_Slab)->Unit(benchmark::kMillisecond);

static void BM_ProducerConsumer_Jemalloc(benchmark::State& state)
{
    for (auto _ : state)
    {
        auto r = run_producer_consumer("jemalloc", []()->void*{return mallocx(MSG_SIZE,0);}, [](Message*m){dallocx(m,0);});
        benchmark::DoNotOptimize(r.messages);
    }
}
BENCHMARK(BM_ProducerConsumer_Jemalloc)->Unit(benchmark::kMillisecond);

static void BM_ProducerConsumer_Malloc(benchmark::State& state)
{
    for (auto _ : state)
    {
        auto r = run_producer_consumer("malloc", []()->void*{return std::malloc(MSG_SIZE);}, [](Message*m){std::free(m);});
        benchmark::DoNotOptimize(r.messages);
    }
}
BENCHMARK(BM_ProducerConsumer_Malloc)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
