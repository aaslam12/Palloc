// ═══════════════════════════════════════════════════════════════════════════════
// Order Book Simulation — Realistic Allocator Benchmark
//
// Simulates a limit order book with continuous order arrival, cancellation,
// execution, and modification. Each order is a real struct with linked-list
// pointers, written and read during book operations. Tests fixed-size
// allocation under realistic churn patterns.
//
// Allocators tested: Pool, Slab (custom config), jemalloc, malloc
// Mode: Single-threaded
// ═══════════════════════════════════════════════════════════════════════════════

#include <benchmark/benchmark.h>
#include "pool.h"
#include "slab.h"

#include <jemalloc/jemalloc.h>
#include <x86intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

using namespace AL;

// ─── Test parameters ─────────────────────────────────────────────────────────

static constexpr int PRICE_RANGE = 1000;
static constexpr int WARMUP_ORDERS = 5000;
static constexpr size_t POOL_CAPACITY   = 500'000;
static constexpr size_t OPS_LIMIT       = 1'000'000;

// ─── Order struct ─────────────────────────────────────────────────────────────

struct Order
{
    uint64_t order_id;
    uint64_t timestamp;
    double price;
    uint32_t quantity;
    uint32_t remaining_qty;
    uint16_t symbol_id;
    uint8_t side;
    uint8_t order_type;
    uint32_t tracker_idx;
    Order* prev;
    Order* next;
};

static_assert(sizeof(Order) <= 64, "Order must fit in 64-byte slab class");

// ─── Custom slab config ───────────────────────────────────────────────────────

constexpr std::array<size_class, 1> order_slab_classes = {
    size_class{.byte_size = 64, .num_blocks = POOL_CAPACITY, .batch_size = 128}};
using order_slab_cfg = slab_config<1, order_slab_classes>;

// ─── Price level ─────────────────────────────────────────────────────────────

struct PriceLevel
{
    Order* head = nullptr;
    Order* tail = nullptr;
    uint32_t count = 0;

    void push_back(Order* ord)
    {
        ord->prev = tail;
        ord->next = nullptr;
        if (tail) tail->next = ord;
        else head = ord;
        tail = ord;
        count++;
    }

    void remove(Order* ord)
    {
        if (ord->prev) ord->prev->next = ord->next;
        else head = ord->next;
        if (ord->next) ord->next->prev = ord->prev;
        else tail = ord->prev;
        count--;
    }

    Order* pop_front()
    {
        if (!head) return nullptr;
        Order* ord = head;
        head = ord->next;
        if (head) head->prev = nullptr;
        else tail = nullptr;
        count--;
        return ord;
    }
};

// ─── Benchmark result ─────────────────────────────────────────────────────────

struct BenchResult
{
    const char* name;
    size_t ops;
    double elapsed_sec;
    uint64_t cycles;
};

// ─── Order book ───────────────────────────────────────────────────────────────

struct OrderBook
{
    PriceLevel bids[PRICE_RANGE];
    PriceLevel asks[PRICE_RANGE];
    std::vector<Order*> live_orders;
    int best_bid = -1;
    int best_ask = PRICE_RANGE;

    void reserve(size_t n) { live_orders.reserve(n); }

    void add_order(Order* ord)
    {
        int price_idx = static_cast<int>(ord->price);
        if (ord->side == 0)
        {
            bids[price_idx].push_back(ord);
            if (price_idx > best_bid) best_bid = price_idx;
        }
        else
        {
            asks[price_idx].push_back(ord);
            if (price_idx < best_ask) best_ask = price_idx;
        }
        ord->tracker_idx = static_cast<uint32_t>(live_orders.size());
        live_orders.push_back(ord);
    }

    void remove_from_tracker(Order* ord)
    {
        uint32_t idx = ord->tracker_idx;
        if (idx != live_orders.size() - 1)
        {
            Order* last = live_orders.back();
            live_orders[idx] = last;
            last->tracker_idx = idx;
        }
        live_orders.pop_back();
    }

    void cancel_order(Order* ord)
    {
        int price_idx = static_cast<int>(ord->price);
        if (ord->side == 0)
        {
            bids[price_idx].remove(ord);
            if (bids[price_idx].count == 0 && price_idx == best_bid)
                while (best_bid >= 0 && bids[best_bid].count == 0) best_bid--;
        }
        else
        {
            asks[price_idx].remove(ord);
            if (asks[price_idx].count == 0 && price_idx == best_ask)
                while (best_ask < PRICE_RANGE && asks[best_ask].count == 0) best_ask++;
        }
        remove_from_tracker(ord);
    }

    Order* pick_random(std::mt19937& rng)
    {
        if (live_orders.empty()) return nullptr;
        size_t idx = rng() % live_orders.size();
        return live_orders[idx];
    }

    template <typename FreeFn>
    size_t execute_top(FreeFn free_fn)
    {
        size_t freed = 0;
        while (best_bid >= 0 && best_ask < PRICE_RANGE && best_bid >= best_ask)
        {
            Order* bid = bids[best_bid].pop_front();
            Order* ask = asks[best_ask].pop_front();
            if (!bid || !ask)
            {
                if (bid) bids[best_bid].push_back(bid);
                if (ask) asks[best_ask].push_back(ask);
                break;
            }

            uint32_t fill_qty = std::min(bid->remaining_qty, ask->remaining_qty);
            benchmark::DoNotOptimize(fill_qty);

            remove_from_tracker(bid);
            remove_from_tracker(ask);
            free_fn(bid);
            free_fn(ask);
            freed += 2;

            if (bids[best_bid].count == 0)
                while (best_bid >= 0 && bids[best_bid].count == 0) best_bid--;
            if (asks[best_ask].count == 0)
                while (best_ask < PRICE_RANGE && asks[best_ask].count == 0) best_ask++;
        }
        return freed;
    }
};

// ─── Single-threaded test runner ──────────────────────────────────────────────

template <typename AllocFn, typename FreeFn>
BenchResult run_st(const char* name, AllocFn alloc_fn, FreeFn free_fn)
{
    OrderBook book;
    book.reserve(POOL_CAPACITY);

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> action_dist(0, 99);
    std::uniform_int_distribution<int> price_dist(100, 899);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);

    uint64_t order_id = 0;
    size_t ops = 0;

    // Warmup: fill the book
    for (int i = 0; i < WARMUP_ORDERS; i++)
    {
        void* mem = alloc_fn();
        if (!mem) break;
        auto* ord = static_cast<Order*>(mem);
        ord->order_id = order_id++;
        ord->timestamp = ops;
        ord->price = price_dist(rng);
        ord->quantity = qty_dist(rng);
        ord->remaining_qty = ord->quantity;
        ord->symbol_id = static_cast<uint16_t>(i % 4);
        ord->side = static_cast<uint8_t>(side_dist(rng));
        ord->order_type = 0;
        ord->prev = nullptr;
        ord->next = nullptr;
        book.add_order(ord);
    }

    auto start = std::chrono::high_resolution_clock::now();
    uint64_t t0 = __rdtsc();

    while (ops < OPS_LIMIT)
    {
        int action = action_dist(rng);

        if (action < 45 || book.live_orders.size() < 100)
        {
            void* mem = alloc_fn();
            if (mem)
            {
                auto* ord = static_cast<Order*>(mem);
                ord->order_id = order_id++;
                ord->timestamp = ops;
                ord->price = price_dist(rng);
                ord->quantity = qty_dist(rng);
                ord->remaining_qty = ord->quantity;
                ord->symbol_id = static_cast<uint16_t>(ops % 4);
                ord->side = static_cast<uint8_t>(side_dist(rng));
                ord->order_type = 0;
                ord->prev = nullptr;
                ord->next = nullptr;
                benchmark::DoNotOptimize(ord);
                book.add_order(ord);
            }
        }
        else if (action < 75)
        {
            Order* ord = book.pick_random(rng);
            if (ord)
            {
                volatile uint64_t oid = ord->order_id;
                volatile double p = ord->price;
                (void)oid; (void)p;
                book.cancel_order(ord);
                free_fn(ord);
            }
        }
        else if (action < 90)
        {
            book.execute_top([&](Order* ord) { free_fn(ord); });
        }
        else
        {
            Order* ord = book.pick_random(rng);
            if (ord)
            {
                book.cancel_order(ord);
                free_fn(ord);
                void* mem = alloc_fn();
                if (mem)
                {
                    auto* new_ord = static_cast<Order*>(mem);
                    new_ord->order_id = order_id++;
                    new_ord->timestamp = ops;
                    new_ord->price = price_dist(rng);
                    new_ord->quantity = qty_dist(rng);
                    new_ord->remaining_qty = new_ord->quantity;
                    new_ord->symbol_id = static_cast<uint16_t>(ops % 4);
                    new_ord->side = static_cast<uint8_t>(side_dist(rng));
                    new_ord->order_type = 0;
                    new_ord->prev = nullptr;
                    new_ord->next = nullptr;
                    benchmark::DoNotOptimize(new_ord);
                    book.add_order(new_ord);
                }
            }
        }

        ops++;
    }

    uint64_t total_cycles = __rdtsc() - t0;
    double total_elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();

    for (Order* ord : book.live_orders)
        free_fn(ord);
    book.live_orders.clear();

    return {name, ops, total_elapsed, total_cycles};
}

// ─── Benchmarks ───────────────────────────────────────────────────────────────

static void BM_OrderBook_Pool_ST(benchmark::State& state)
{
    uint64_t total_cycles = 0;
    for (auto _ : state)
    {
        pool<> p(sizeof(Order), POOL_CAPACITY);
        auto r = run_st("Pool", [&]()->void*{return p.alloc();}, [&](Order*o){p.free(o);});
        total_cycles += r.cycles;
        benchmark::DoNotOptimize(r.ops);
    }
    state.counters["cycles/op"] = (double)total_cycles / (double)(state.iterations() * OPS_LIMIT);
}
BENCHMARK(BM_OrderBook_Pool_ST)->Unit(benchmark::kMillisecond);

static void BM_OrderBook_Slab_ST(benchmark::State& state)
{
    uint64_t total_cycles = 0;
    for (auto _ : state)
    {
        slab<order_slab_cfg> s{};
        auto r = run_st("Slab", [&]()->void*{return s.palloc(sizeof(Order));}, [&](Order*o){s.free(o,sizeof(Order));});
        total_cycles += r.cycles;
        benchmark::DoNotOptimize(r.ops);
    }
    state.counters["cycles/op"] = (double)total_cycles / (double)(state.iterations() * OPS_LIMIT);
}
BENCHMARK(BM_OrderBook_Slab_ST)->Unit(benchmark::kMillisecond);

static void BM_OrderBook_Jemalloc_ST(benchmark::State& state)
{
    uint64_t total_cycles = 0;
    for (auto _ : state)
    {
        auto r = run_st("jemalloc", []()->void*{return mallocx(sizeof(Order),0);}, [](Order*o){dallocx(o,0);});
        total_cycles += r.cycles;
        benchmark::DoNotOptimize(r.ops);
    }
    state.counters["cycles/op"] = (double)total_cycles / (double)(state.iterations() * OPS_LIMIT);
}
BENCHMARK(BM_OrderBook_Jemalloc_ST)->Unit(benchmark::kMillisecond);

static void BM_OrderBook_Malloc_ST(benchmark::State& state)
{
    uint64_t total_cycles = 0;
    for (auto _ : state)
    {
        auto r = run_st("malloc", []()->void*{return std::malloc(sizeof(Order));}, [](Order*o){std::free(o);});
        total_cycles += r.cycles;
        benchmark::DoNotOptimize(r.ops);
    }
    state.counters["cycles/op"] = (double)total_cycles / (double)(state.iterations() * OPS_LIMIT);
}
BENCHMARK(BM_OrderBook_Malloc_ST)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
