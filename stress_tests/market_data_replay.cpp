// ═══════════════════════════════════════════════════════════════════════════════
// Market Data Replay — Realistic Allocator Benchmark
//
// Simulates a market data feed handler receiving variable-size messages
// (quotes, trades, L2 snapshots) in batches. Two processing modes:
//   - Batch mode (Arena): allocate entire batch, process, reset
//   - Individual mode (Slab/DynSlab/malloc/jemalloc): alloc each, process, free each
//
// Message data is written and read during processing (VWAP, spread, volume
// accumulation) to exercise real cache/TLB behavior.
//
// Allocators tested: Arena (batch), Slab, Dynamic Slab, jemalloc, malloc
// Mode: Single-threaded (realistic: one feed handler per symbol)
// ═══════════════════════════════════════════════════════════════════════════════

#include "arena.h"
#include "dynamic_slab.h"
#include "slab.h"

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

static constexpr int DURATION_SECS = 7;
static constexpr size_t BATCH_SIZE = 200;
static constexpr size_t LATENCY_CAPACITY = 2'000'000;

// Message sizes matching slab size classes
static constexpr size_t QUOTE_SIZE = 64;     // 60% of messages
static constexpr size_t TRADE_SIZE = 128;    // 30% of messages
static constexpr size_t SNAPSHOT_SIZE = 512; // 10% of messages
static constexpr size_t ARENA_CAPACITY = 256 * 1024; // 256 KB per batch cycle

// ─── Message structures ──────────────────────────────────────────────────────

struct QuoteMsg
{
    uint64_t timestamp;
    uint32_t symbol_id;
    uint32_t sequence;
    double bid_price;
    double ask_price;
    uint32_t bid_size;
    uint32_t ask_size;
    uint8_t venue_id;
    uint8_t _pad[7];
};
static_assert(sizeof(QuoteMsg) <= QUOTE_SIZE);

struct TradeMsg
{
    uint64_t timestamp;
    uint64_t trade_id;
    uint32_t symbol_id;
    uint32_t sequence;
    double price;
    uint32_t size;
    uint8_t aggressor_side;
    uint8_t venue_id;
    uint16_t _pad;
    uint64_t match_id;
    uint64_t buyer_order_id;
    uint64_t seller_order_id;
    uint64_t _pad2;
};
static_assert(sizeof(TradeMsg) <= TRADE_SIZE);

struct BookLevel
{
    double price;
    uint32_t size;
    uint32_t num_orders;
};

struct SnapshotMsg
{
    uint64_t timestamp;
    uint32_t symbol_id;
    uint32_t sequence;
    uint32_t num_levels;
    uint32_t _pad;
    BookLevel bids[15];
    BookLevel asks[15];
};
static_assert(sizeof(SnapshotMsg) <= SNAPSHOT_SIZE);

// ─── Running statistics (accumulated during processing) ──────────────────────

struct FeedStats
{
    uint64_t quote_count = 0;
    uint64_t trade_count = 0;
    uint64_t snapshot_count = 0;
    double vwap_numerator = 0;
    double vwap_denominator = 0;
    double total_spread = 0;
    uint64_t total_volume = 0;
    double last_mid = 0;
};

// ─── Message generation and processing ───────────────────────────────────────

inline size_t pick_msg_size(std::mt19937& rng)
{
    uint32_t r = rng() % 100;
    if (r < 60) return QUOTE_SIZE;
    if (r < 90) return TRADE_SIZE;
    return SNAPSHOT_SIZE;
}

inline void fill_and_process(void* mem, size_t size, uint64_t seq, FeedStats& stats)
{
    std::memset(mem, 0, size);

    if (size == QUOTE_SIZE)
    {
        auto* q = static_cast<QuoteMsg*>(mem);
        q->timestamp = seq;
        q->symbol_id = seq % 100;
        q->sequence = static_cast<uint32_t>(seq);
        q->bid_price = 100.0 + (seq % 1000) * 0.01;
        q->ask_price = q->bid_price + 0.01;
        q->bid_size = 100 + (seq % 500);
        q->ask_size = 100 + (seq % 500);
        q->venue_id = seq % 4;
        escape(q);

        // Process: compute mid and spread
        double mid = (q->bid_price + q->ask_price) * 0.5;
        double spread = q->ask_price - q->bid_price;
        stats.total_spread += spread;
        stats.last_mid = mid;
        stats.quote_count++;
        clobber();
    }
    else if (size == TRADE_SIZE)
    {
        auto* t = static_cast<TradeMsg*>(mem);
        t->timestamp = seq;
        t->trade_id = seq * 7 + 13;
        t->symbol_id = seq % 100;
        t->sequence = static_cast<uint32_t>(seq);
        t->price = 100.0 + (seq % 1000) * 0.01;
        t->size = 1 + (seq % 1000);
        t->aggressor_side = seq % 2;
        t->match_id = seq * 3;
        escape(t);

        // Process: accumulate VWAP
        stats.vwap_numerator += t->price * t->size;
        stats.vwap_denominator += t->size;
        stats.total_volume += t->size;
        stats.trade_count++;
        clobber();
    }
    else
    {
        auto* s = static_cast<SnapshotMsg*>(mem);
        s->timestamp = seq;
        s->symbol_id = seq % 100;
        s->sequence = static_cast<uint32_t>(seq);
        s->num_levels = 10;
        for (uint32_t i = 0; i < 10; i++)
        {
            s->bids[i] = {100.0 - i * 0.01, 100u + i * 10, 5u + i};
            s->asks[i] = {100.01 + i * 0.01, 100u + i * 10, 5u + i};
        }
        escape(s);

        // Process: compute total book depth
        double total_bid_depth = 0;
        for (uint32_t i = 0; i < s->num_levels; i++)
            total_bid_depth += s->bids[i].size;
        stats.total_volume += static_cast<uint64_t>(total_bid_depth);
        stats.snapshot_count++;
        clobber();
    }
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

struct BenchResult
{
    const char* name;
    size_t batches;
    size_t messages;
    double elapsed_sec;
    LatencyRecorder::Stats latency;
};

// ─── Print helpers ───────────────────────────────────────────────────────────

void print_results(const std::vector<BenchResult>& results)
{
    printf("\n  %-22s %10s %10s %12s\n", "Allocator", "ns/msg", "ns/batch", "MOps/s");
    printf("  ─────────────────────────────────────────────────────────\n");
    for (const auto& r : results)
    {
        double ns_msg = (r.elapsed_sec * 1e9) / static_cast<double>(r.messages);
        double ns_batch = (r.elapsed_sec * 1e9) / static_cast<double>(r.batches);
        double mops = static_cast<double>(r.messages) / r.elapsed_sec / 1e6;
        printf("  %-22s %8.1f %10.0f %12.1f\n", r.name, ns_msg, ns_batch, mops);
    }

    printf("\n  Batch latency distribution (ns per batch of %zu messages):\n", BATCH_SIZE);
    printf("  %-22s %8s %8s %8s %8s %8s\n", "Allocator", "p50", "p90", "p99", "p99.9", "mean");
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
    printf("║    Market Data Replay — Realistic Allocator Benchmark      ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  Messages: 60%% quote (%zuB), 30%% trade (%zuB), 10%% snap (%zuB)║\n",
           QUOTE_SIZE, TRADE_SIZE, SNAPSHOT_SIZE);
    printf("║  Batch size: %zu messages, Duration: %d seconds              ║\n",
           BATCH_SIZE, DURATION_SECS);
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    std::vector<BenchResult> results;

    // ── Arena (batch mode): alloc all, process all, reset ────────────────
    {
        arena a(ARENA_CAPACITY);
        FeedStats stats{};
        LatencyRecorder recorder(LATENCY_CAPACITY);
        std::mt19937 rng(42);
        size_t batches = 0, messages = 0;
        uint64_t seq = 0;

        auto start = Clock::now();
        auto deadline = start + std::chrono::seconds(DURATION_SECS);

        while (Clock::now() < deadline)
        {
            bool sample = (batches & 15) == 0;
            auto t0 = sample ? Clock::now() : Clock::time_point{};

            // Allocate and process entire batch
            for (size_t i = 0; i < BATCH_SIZE; i++)
            {
                size_t sz = pick_msg_size(rng);
                void* mem = a.alloc(sz);
                if (!mem)
                {
                    a.reset();
                    mem = a.alloc(sz);
                }
                if (mem)
                {
                    fill_and_process(mem, sz, seq++, stats);
                    messages++;
                }
            }
            a.reset();

            if (sample)
            {
                auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - t0).count();
                recorder.record(static_cast<uint64_t>(elapsed));
            }
            batches++;
        }

        double total_elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        results.push_back({"Arena (batch)", batches, messages, total_elapsed, recorder.compute()});
        escape(&stats);
    }

    // ── Slab (individual mode): alloc each, process, free each ───────────
    {
        default_slab s{};
        FeedStats stats{};
        LatencyRecorder recorder(LATENCY_CAPACITY);
        std::mt19937 rng(42);
        size_t batches = 0, messages = 0;
        uint64_t seq = 0;

        struct Entry
        {
            void* ptr;
            size_t size;
        };
        std::vector<Entry> batch(BATCH_SIZE);

        auto start = Clock::now();
        auto deadline = start + std::chrono::seconds(DURATION_SECS);

        while (Clock::now() < deadline)
        {
            bool sample = (batches & 15) == 0;
            auto t0 = sample ? Clock::now() : Clock::time_point{};

            size_t count = 0;
            for (size_t i = 0; i < BATCH_SIZE; i++)
            {
                size_t sz = pick_msg_size(rng);
                void* mem = s.alloc(sz);
                if (mem)
                {
                    fill_and_process(mem, sz, seq++, stats);
                    batch[count++] = {mem, sz};
                    messages++;
                }
            }
            for (size_t i = 0; i < count; i++)
                s.free(batch[i].ptr, batch[i].size);

            if (sample)
            {
                auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - t0).count();
                recorder.record(static_cast<uint64_t>(elapsed));
            }
            batches++;
        }

        double total_elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        results.push_back({"Slab (TLC)", batches, messages, total_elapsed, recorder.compute()});
        escape(&stats);
    }

    // ── Dynamic Slab ─────────────────────────────────────────────────────
    {
        default_dynamic_slab ds{};
        FeedStats stats{};
        LatencyRecorder recorder(LATENCY_CAPACITY);
        std::mt19937 rng(42);
        size_t batches = 0, messages = 0;
        uint64_t seq = 0;

        struct Entry
        {
            void* ptr;
            size_t size;
        };
        std::vector<Entry> batch(BATCH_SIZE);

        auto start = Clock::now();
        auto deadline = start + std::chrono::seconds(DURATION_SECS);

        while (Clock::now() < deadline)
        {
            bool sample = (batches & 15) == 0;
            auto t0 = sample ? Clock::now() : Clock::time_point{};

            size_t count = 0;
            for (size_t i = 0; i < BATCH_SIZE; i++)
            {
                size_t sz = pick_msg_size(rng);
                void* mem = ds.palloc(sz);
                if (mem)
                {
                    fill_and_process(mem, sz, seq++, stats);
                    batch[count++] = {mem, sz};
                    messages++;
                }
            }
            for (size_t i = 0; i < count; i++)
                ds.free(batch[i].ptr, batch[i].size);

            if (sample)
            {
                auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - t0).count();
                recorder.record(static_cast<uint64_t>(elapsed));
            }
            batches++;
        }

        double total_elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        results.push_back({"Dynamic Slab", batches, messages, total_elapsed, recorder.compute()});
        escape(&stats);
    }

    // ── jemalloc ─────────────────────────────────────────────────────────
    {
        FeedStats stats{};
        LatencyRecorder recorder(LATENCY_CAPACITY);
        std::mt19937 rng(42);
        size_t batches = 0, messages = 0;
        uint64_t seq = 0;

        struct Entry
        {
            void* ptr;
            size_t size;
        };
        std::vector<Entry> batch(BATCH_SIZE);

        auto start = Clock::now();
        auto deadline = start + std::chrono::seconds(DURATION_SECS);

        while (Clock::now() < deadline)
        {
            bool sample = (batches & 15) == 0;
            auto t0 = sample ? Clock::now() : Clock::time_point{};

            size_t count = 0;
            for (size_t i = 0; i < BATCH_SIZE; i++)
            {
                size_t sz = pick_msg_size(rng);
                void* mem = mallocx(sz, 0);
                if (mem)
                {
                    fill_and_process(mem, sz, seq++, stats);
                    batch[count++] = {mem, sz};
                    messages++;
                }
            }
            for (size_t i = 0; i < count; i++)
                dallocx(batch[i].ptr, 0);

            if (sample)
            {
                auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - t0).count();
                recorder.record(static_cast<uint64_t>(elapsed));
            }
            batches++;
        }

        double total_elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        results.push_back({"jemalloc", batches, messages, total_elapsed, recorder.compute()});
        escape(&stats);
    }

    // ── glibc malloc ─────────────────────────────────────────────────────
    {
        FeedStats stats{};
        LatencyRecorder recorder(LATENCY_CAPACITY);
        std::mt19937 rng(42);
        size_t batches = 0, messages = 0;
        uint64_t seq = 0;

        struct Entry
        {
            void* ptr;
            size_t size;
        };
        std::vector<Entry> batch(BATCH_SIZE);

        auto start = Clock::now();
        auto deadline = start + std::chrono::seconds(DURATION_SECS);

        while (Clock::now() < deadline)
        {
            bool sample = (batches & 15) == 0;
            auto t0 = sample ? Clock::now() : Clock::time_point{};

            size_t count = 0;
            for (size_t i = 0; i < BATCH_SIZE; i++)
            {
                size_t sz = pick_msg_size(rng);
                void* mem = std::malloc(sz);
                if (mem)
                {
                    fill_and_process(mem, sz, seq++, stats);
                    batch[count++] = {mem, sz};
                    messages++;
                }
            }
            for (size_t i = 0; i < count; i++)
                std::free(batch[i].ptr);

            if (sample)
            {
                auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - t0).count();
                recorder.record(static_cast<uint64_t>(elapsed));
            }
            batches++;
        }

        double total_elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        results.push_back({"malloc", batches, messages, total_elapsed, recorder.compute()});
        escape(&stats);
    }

    printf("\n━━━ Market Data Feed Processing (batch of %zu, %ds each) ━━━\n", BATCH_SIZE, DURATION_SECS);
    print_results(results);

    return 0;
}
