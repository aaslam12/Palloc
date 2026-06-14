// ═══════════════════════════════════════════════════════════════════════════════
// Market Data Replay — Realistic Allocator Benchmark
//
// Simulates a market data feed handler receiving variable-size messages
// (quotes, trades, L2 snapshots) in batches. Two processing modes:
//   - Batch mode (Arena): allocate entire batch, process, reset
//   - Individual mode (Slab/malloc/jemalloc): alloc each, process, free each
//
// Allocators tested: Arena (batch), Slab, jemalloc, malloc
// Mode: Single-threaded (realistic: one feed handler per symbol)
// ═══════════════════════════════════════════════════════════════════════════════

#include <benchmark/benchmark.h>
#include "arena.h"
#include "slab.h"

#include <jemalloc/jemalloc.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace AL;

// ─── Test parameters ─────────────────────────────────────────────────────────

static constexpr size_t BATCH_SIZE = 200;
static constexpr size_t QUOTE_SIZE = 64;
static constexpr size_t TRADE_SIZE = 128;
static constexpr size_t SNAPSHOT_SIZE = 512;
static constexpr size_t ARENA_CAPACITY = 256 * 1024;

// ─── Message structures ───────────────────────────────────────────────────────

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

// ─── Running statistics ───────────────────────────────────────────────────────

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
        benchmark::DoNotOptimize(q);

        double mid = (q->bid_price + q->ask_price) * 0.5;
        double spread = q->ask_price - q->bid_price;
        stats.total_spread += spread;
        stats.last_mid = mid;
        stats.quote_count++;
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
        benchmark::DoNotOptimize(t);

        stats.vwap_numerator += t->price * t->size;
        stats.vwap_denominator += t->size;
        stats.total_volume += t->size;
        stats.trade_count++;
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
        benchmark::DoNotOptimize(s);

        double total_bid_depth = 0;
        for (uint32_t i = 0; i < s->num_levels; i++)
            total_bid_depth += s->bids[i].size;
        stats.total_volume += static_cast<uint64_t>(total_bid_depth);
        stats.snapshot_count++;
    }
}

// ─── Benchmarks ───────────────────────────────────────────────────────────────

static void BM_MarketData_Arena(benchmark::State& state)
{
    arena<> a(ARENA_CAPACITY);
    FeedStats stats{};
    std::mt19937 rng(42);
    uint64_t seq = 0;
    for (auto _ : state)
    {
        for (size_t i = 0; i < BATCH_SIZE; ++i)
        {
            size_t sz = pick_msg_size(rng);
            void* mem = a.alloc(sz);
            if (!mem) { a.reset(); mem = a.alloc(sz); }
            if (mem) { fill_and_process(mem, sz, seq++, stats); }
        }
        a.reset();
    }
    state.SetItemsProcessed(state.iterations() * BATCH_SIZE);
    benchmark::DoNotOptimize(stats);
}
BENCHMARK(BM_MarketData_Arena);

static void BM_MarketData_Slab(benchmark::State& state)
{
    default_slab s{};
    FeedStats stats{};
    std::mt19937 rng(42);
    uint64_t seq = 0;
    struct Entry { void* ptr; size_t size; };
    std::vector<Entry> batch(BATCH_SIZE);
    for (auto _ : state)
    {
        size_t count = 0;
        for (size_t i = 0; i < BATCH_SIZE; ++i)
        {
            size_t sz = pick_msg_size(rng);
            void* mem = s.palloc(sz);
            if (mem) { fill_and_process(mem, sz, seq++, stats); batch[count++] = {mem, sz}; }
        }
        for (size_t i = 0; i < count; ++i) s.free(batch[i].ptr, batch[i].size);
    }
    state.SetItemsProcessed(state.iterations() * BATCH_SIZE);
    benchmark::DoNotOptimize(stats);
}
BENCHMARK(BM_MarketData_Slab);

static void BM_MarketData_Jemalloc(benchmark::State& state)
{
    FeedStats stats{};
    std::mt19937 rng(42);
    uint64_t seq = 0;
    struct Entry { void* ptr; size_t size; };
    std::vector<Entry> batch(BATCH_SIZE);
    for (auto _ : state)
    {
        size_t count = 0;
        for (size_t i = 0; i < BATCH_SIZE; ++i)
        {
            size_t sz = pick_msg_size(rng);
            void* mem = mallocx(sz, 0);
            if (mem) { fill_and_process(mem, sz, seq++, stats); batch[count++] = {mem, sz}; }
        }
        for (size_t i = 0; i < count; ++i) dallocx(batch[i].ptr, 0);
    }
    state.SetItemsProcessed(state.iterations() * BATCH_SIZE);
    benchmark::DoNotOptimize(stats);
}
BENCHMARK(BM_MarketData_Jemalloc);

static void BM_MarketData_Malloc(benchmark::State& state)
{
    FeedStats stats{};
    std::mt19937 rng(42);
    uint64_t seq = 0;
    struct Entry { void* ptr; size_t size; };
    std::vector<Entry> batch(BATCH_SIZE);
    for (auto _ : state)
    {
        size_t count = 0;
        for (size_t i = 0; i < BATCH_SIZE; ++i)
        {
            size_t sz = pick_msg_size(rng);
            void* mem = std::malloc(sz);
            if (mem) { fill_and_process(mem, sz, seq++, stats); batch[count++] = {mem, sz}; }
        }
        for (size_t i = 0; i < count; ++i) std::free(batch[i].ptr);
    }
    state.SetItemsProcessed(state.iterations() * BATCH_SIZE);
    benchmark::DoNotOptimize(stats);
}
BENCHMARK(BM_MarketData_Malloc);

BENCHMARK_MAIN();
