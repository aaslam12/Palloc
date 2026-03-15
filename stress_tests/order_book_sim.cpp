// ═══════════════════════════════════════════════════════════════════════════════
// Order Book Simulation — Realistic Allocator Benchmark
//
// Simulates a limit order book with continuous order arrival, cancellation,
// execution, and modification. Each order is a real struct with linked-list
// pointers, written and read during book operations. Tests fixed-size
// allocation under realistic churn patterns.
//
// Allocators tested: Pool, Slab (custom config), Dynamic Slab, jemalloc, malloc
// Modes: Single-threaded and Multi-threaded (shared allocator, per-thread books)
// ═══════════════════════════════════════════════════════════════════════════════

#include "dynamic_slab.h"
#include "pool.h"
#include "slab.h"

#include <jemalloc/jemalloc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

using namespace AL;
using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::nanoseconds;

// ─── Compiler hints ──────────────────────────────────────────────────────────

inline void escape(void* p) { asm volatile("" : : "g"(p) : "memory"); }
inline void clobber() { asm volatile("" : : : "memory"); }

// ─── Test parameters ─────────────────────────────────────────────────────────

static constexpr int PRICE_RANGE = 1000;
static constexpr int WARMUP_ORDERS = 5000;
static constexpr int DURATION_SECS = 7;
static constexpr size_t LATENCY_CAPACITY = 2'000'000;
static constexpr size_t POOL_CAPACITY = 500'000;

// ─── Order struct — realistic trading order ──────────────────────────────────
// Laid out to match a real order entry with doubly-linked list pointers
// for O(1) insert/remove at price levels. sizeof(Order) = 64 bytes.

struct Order
{
    uint64_t order_id;
    uint64_t timestamp;
    double price;
    uint32_t quantity;
    uint32_t remaining_qty;
    uint16_t symbol_id;
    uint8_t side;       // 0 = bid, 1 = ask
    uint8_t order_type; // 0 = limit
    uint32_t tracker_idx;
    Order* prev;
    Order* next;
};

static_assert(sizeof(Order) <= 64, "Order must fit in 64-byte slab class");

// ─── Custom slab config: single 64B class with high capacity ─────────────────

constexpr std::array<size_class, 1> order_slab_classes = {
    size_class{.byte_size = 64, .num_blocks = POOL_CAPACITY, .batch_size = 128}};
using order_slab_cfg = slab_config<1, order_slab_classes>;

// ─── Price level (doubly-linked list of orders at one price) ─────────────────

struct PriceLevel
{
    Order* head = nullptr;
    Order* tail = nullptr;
    uint32_t count = 0;

    void push_back(Order* ord)
    {
        ord->prev = tail;
        ord->next = nullptr;
        if (tail)
            tail->next = ord;
        else
            head = ord;
        tail = ord;
        count++;
    }

    void remove(Order* ord)
    {
        if (ord->prev)
            ord->prev->next = ord->next;
        else
            head = ord->next;
        if (ord->next)
            ord->next->prev = ord->prev;
        else
            tail = ord->prev;
        count--;
    }

    Order* pop_front()
    {
        if (!head) return nullptr;
        Order* ord = head;
        head = ord->next;
        if (head)
            head->prev = nullptr;
        else
            tail = nullptr;
        count--;
        return ord;
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
    size_t ops;
    double elapsed_sec;
    LatencyRecorder::Stats latency;
};

// ─── Order book ──────────────────────────────────────────────────────────────

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

    // Execute crossing orders at the top of the book.
    // Returns number of orders freed.
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

            // Simulate fill
            uint32_t fill_qty = std::min(bid->remaining_qty, ask->remaining_qty);
            escape(&fill_qty);

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

// ─── Single-threaded test runner ─────────────────────────────────────────────

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

    LatencyRecorder recorder(LATENCY_CAPACITY);
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

    auto start = Clock::now();
    auto deadline = start + std::chrono::seconds(DURATION_SECS);

    while (Clock::now() < deadline)
    {
        bool sample = (ops & 127) == 0;
        auto t0 = sample ? Clock::now() : Clock::time_point{};

        int action = action_dist(rng);

        if (action < 45 || book.live_orders.size() < 100)
        {
            // ADD ORDER (45%)
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
                escape(ord);
                clobber();
                book.add_order(ord);
            }
        }
        else if (action < 75)
        {
            // CANCEL ORDER (30%)
            Order* ord = book.pick_random(rng);
            if (ord)
            {
                volatile uint64_t oid = ord->order_id;
                volatile double p = ord->price;
                (void)oid;
                (void)p;
                book.cancel_order(ord);
                free_fn(ord);
            }
        }
        else if (action < 90)
        {
            // EXECUTE TOP OF BOOK (15%)
            book.execute_top([&](Order* ord) { free_fn(ord); });
        }
        else
        {
            // MODIFY ORDER (10%): cancel + re-add with new price
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
                    escape(new_ord);
                    clobber();
                    book.add_order(new_ord);
                }
            }
        }

        if (sample)
        {
            auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - t0).count();
            recorder.record(static_cast<uint64_t>(elapsed));
        }
        ops++;
    }

    double total_elapsed = std::chrono::duration<double>(Clock::now() - start).count();

    // Cleanup
    for (Order* ord : book.live_orders)
        free_fn(ord);
    book.live_orders.clear();

    return {name, ops, total_elapsed, recorder.compute()};
}

// ─── Multi-threaded test runner ──────────────────────────────────────────────
// Each thread operates its own order book, all sharing one allocator.
// This models per-symbol processing on separate cores.

template <typename AllocFn, typename FreeFn>
BenchResult run_mt(const char* name, size_t num_threads, AllocFn alloc_fn, FreeFn free_fn)
{
    std::atomic<bool> go{false};
    std::atomic<size_t> total_ops{0};
    std::vector<LatencyRecorder> recorders;
    for (size_t i = 0; i < num_threads; i++)
        recorders.emplace_back(LATENCY_CAPACITY / num_threads);

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    auto start = Clock::now();

    for (size_t tid = 0; tid < num_threads; tid++)
    {
        threads.emplace_back([&, tid] {
            while (!go.load(std::memory_order_acquire))
                ;

            OrderBook book;
            book.reserve(POOL_CAPACITY / num_threads);

            std::mt19937 rng(42 + tid);
            std::uniform_int_distribution<int> action_dist(0, 99);
            std::uniform_int_distribution<int> price_dist(100, 899);
            std::uniform_int_distribution<uint32_t> qty_dist(1, 1000);
            std::uniform_int_distribution<int> side_dist(0, 1);

            uint64_t order_id = tid * 100'000'000;
            size_t ops = 0;

            // Warmup
            for (int i = 0; i < WARMUP_ORDERS / static_cast<int>(num_threads); i++)
            {
                void* mem = alloc_fn();
                if (!mem) break;
                auto* ord = static_cast<Order*>(mem);
                ord->order_id = order_id++;
                ord->timestamp = ops;
                ord->price = price_dist(rng);
                ord->quantity = qty_dist(rng);
                ord->remaining_qty = ord->quantity;
                ord->symbol_id = static_cast<uint16_t>(tid);
                ord->side = static_cast<uint8_t>(side_dist(rng));
                ord->order_type = 0;
                ord->prev = nullptr;
                ord->next = nullptr;
                book.add_order(ord);
            }

            auto deadline = Clock::now() + std::chrono::seconds(DURATION_SECS);

            while (Clock::now() < deadline)
            {
                bool sample = (ops & 255) == 0;
                auto t0 = sample ? Clock::now() : Clock::time_point{};
                int action = action_dist(rng);

                if (action < 45 || book.live_orders.size() < 50)
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
                        ord->symbol_id = static_cast<uint16_t>(tid);
                        ord->side = static_cast<uint8_t>(side_dist(rng));
                        ord->order_type = 0;
                        ord->prev = nullptr;
                        ord->next = nullptr;
                        escape(ord);
                        clobber();
                        book.add_order(ord);
                    }
                }
                else if (action < 75)
                {
                    Order* ord = book.pick_random(rng);
                    if (ord)
                    {
                        escape(ord);
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
                            new_ord->symbol_id = static_cast<uint16_t>(tid);
                            new_ord->side = static_cast<uint8_t>(side_dist(rng));
                            new_ord->order_type = 0;
                            new_ord->prev = nullptr;
                            new_ord->next = nullptr;
                            escape(new_ord);
                            clobber();
                            book.add_order(new_ord);
                        }
                    }
                }

                if (sample)
                {
                    auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - t0).count();
                    recorders[tid].record(static_cast<uint64_t>(elapsed));
                }
                ops++;
            }

            // Cleanup
            for (Order* ord : book.live_orders)
                free_fn(ord);
            book.live_orders.clear();

            total_ops.fetch_add(ops, std::memory_order_relaxed);
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& t : threads)
        t.join();

    double total_elapsed = std::chrono::duration<double>(Clock::now() - start).count();

    // Merge latency samples from all threads
    LatencyRecorder merged(LATENCY_CAPACITY);
    for (auto& rec : recorders)
        for (size_t i = 0; i < rec.idx; i++)
            merged.record(rec.samples[i]);

    return {name, total_ops.load(), total_elapsed, merged.compute()};
}

// ─── Print helpers ───────────────────────────────────────────────────────────

void print_results(const char* title, const std::vector<BenchResult>& results)
{
    printf("\n━━━ %s ━━━\n\n", title);

    printf("  %-22s %10s %12s\n", "Allocator", "ns/op", "MOps/s");
    printf("  ──────────────────────────────────────────────\n");
    for (const auto& r : results)
    {
        double ns = (r.elapsed_sec * 1e9) / static_cast<double>(r.ops);
        double mops = static_cast<double>(r.ops) / r.elapsed_sec / 1e6;
        printf("  %-22s %8.1f %12.1f\n", r.name, ns, mops);
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
    printf("║     Order Book Simulation — Realistic Allocator Benchmark  ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  Ops: 45%% add, 30%% cancel, 15%% execute, 10%% modify       ║\n");
    printf("║  Order struct: %zu bytes, linked-list book operations       ║\n", sizeof(Order));
    printf("║  Duration: %d seconds per allocator                         ║\n", DURATION_SECS);
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    constexpr size_t order_size = sizeof(Order);

    // ── Single-threaded ──────────────────────────────────────────────────
    {
        std::vector<BenchResult> results;

        {
            pool p(order_size, POOL_CAPACITY);
            results.push_back(run_st(
                "Pool",
                [&]() -> void* { return p.alloc(); },
                [&](Order* o) { p.free(o); }));
        }
        {
            slab<order_slab_cfg> s{};
            results.push_back(run_st(
                "Slab (TLC)",
                [&]() -> void* { return s.alloc(order_size); },
                [&](Order* o) { s.free(o, order_size); }));
        }
        {
            default_dynamic_slab ds{};
            results.push_back(run_st(
                "Dynamic Slab",
                [&]() -> void* { return ds.palloc(order_size); },
                [&](Order* o) { ds.free(o, order_size); }));
        }
        {
            results.push_back(run_st(
                "jemalloc",
                []() -> void* { return mallocx(order_size, 0); },
                [](Order* o) { dallocx(o, 0); }));
        }
        {
            results.push_back(run_st(
                "malloc",
                []() -> void* { return std::malloc(order_size); },
                [](Order* o) { std::free(o); }));
        }

        print_results("Single-Threaded Order Book", results);
    }

    // ── Multi-threaded ───────────────────────────────────────────────────
    {
        size_t num_threads = std::min<size_t>(std::thread::hardware_concurrency(), 8);
        if (num_threads < 2) num_threads = 2;

        char title[128];
        std::snprintf(title, sizeof(title),
                      "Multi-Threaded Order Book (%zu threads, shared allocator)", num_threads);

        std::vector<BenchResult> results;

        {
            pool p(order_size, POOL_CAPACITY);
            results.push_back(run_mt(
                "Pool", num_threads,
                [&]() -> void* { return p.alloc(); },
                [&](Order* o) { p.free(o); }));
        }
        {
            slab<order_slab_cfg> s{};
            results.push_back(run_mt(
                "Slab (TLC)", num_threads,
                [&]() -> void* { return s.alloc(order_size); },
                [&](Order* o) { s.free(o, order_size); }));
        }
        {
            default_dynamic_slab ds{};
            results.push_back(run_mt(
                "Dynamic Slab", num_threads,
                [&]() -> void* { return ds.palloc(order_size); },
                [&](Order* o) { ds.free(o, order_size); }));
        }
        {
            results.push_back(run_mt(
                "jemalloc", num_threads,
                []() -> void* { return mallocx(order_size, 0); },
                [](Order* o) { dallocx(o, 0); }));
        }
        {
            results.push_back(run_mt(
                "malloc", num_threads,
                []() -> void* { return std::malloc(order_size); },
                [](Order* o) { std::free(o); }));
        }

        print_results(title, results);
    }

    return 0;
}
