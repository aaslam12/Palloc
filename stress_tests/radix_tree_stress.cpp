#include <benchmark/benchmark.h>
#include "radix_tree.h"
#include <cstdlib>
#include <random>
#include <vector>

static void* addr(uintptr_t v) { return reinterpret_cast<void*>(v); }

struct RadixFixture : benchmark::Fixture
{
    AL::radix_tree rt;
    struct RangeInfo { uintptr_t start, end; };
    std::vector<RangeInfo> ranges;
    static constexpr size_t N = 100000;
    static constexpr uintptr_t RSIZE = 0x1000, GAP = 0x1000;

    void SetUp(const benchmark::State&) override
    {
        if (!ranges.empty()) return;
        ranges.reserve(N);
        for (size_t i = 0; i < N; ++i)
        {
            uintptr_t s = 0x100000 + i * (RSIZE + GAP);
            rt.insert(addr(s), addr(s + RSIZE), i + 1);
            ranges.push_back({s, s + RSIZE});
        }
    }
};

BENCHMARK_DEFINE_F(RadixFixture, Insert)(benchmark::State& state)
{
    AL::radix_tree local_rt;
    for (auto _ : state)
    {
        for (size_t i = 0; i < N; ++i)
        {
            uintptr_t s = 0x200000 + i * (RSIZE + GAP);
            local_rt.insert(addr(s), addr(s + RSIZE), i + 1);
        }
        state.PauseTiming();
        local_rt.clear();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * N);
}
BENCHMARK_REGISTER_F(RadixFixture, Insert)->Unit(benchmark::kMillisecond);

BENCHMARK_DEFINE_F(RadixFixture, LookupHit)(benchmark::State& state)
{
    size_t i = 0;
    for (auto _ : state)
    {
        const auto& r = ranges[i % N];
        size_t res = rt.lookup(addr(r.start + RSIZE / 2));
        benchmark::DoNotOptimize(res);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK_REGISTER_F(RadixFixture, LookupHit);

BENCHMARK_DEFINE_F(RadixFixture, LookupMiss)(benchmark::State& state)
{
    size_t i = 0;
    for (auto _ : state)
    {
        const auto& r = ranges[i % N];
        size_t res = rt.lookup(addr(r.end + GAP / 2));
        benchmark::DoNotOptimize(res);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK_REGISTER_F(RadixFixture, LookupMiss);

BENCHMARK_MAIN();
