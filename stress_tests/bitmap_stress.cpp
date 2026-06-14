#include <benchmark/benchmark.h>
#include "bitmap.h"
#include <algorithm>
#include <vector>
using namespace AL;

static void BM_Bitmap_AllocFree(benchmark::State& state)
{
    const size_t num_slots = static_cast<size_t>(state.threads()) * 65536;
    std::vector<uint8_t> buf(bitmap<>::required_size(num_slots), 0);
    bitmap<> bm;
    bm.init(buf.data(), num_slots);
    for (auto _ : state)
    {
        size_t s = bm.alloc_bit();
        if (s != static_cast<size_t>(-1))
            bm.free_bit(s);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Bitmap_AllocFree)->ThreadRange(1, 16);

static void BM_Bitmap_BatchChurn(benchmark::State& state)
{
    const size_t num_slots = static_cast<size_t>(state.threads()) * 65536;
    const size_t batch = 64;
    std::vector<uint8_t> buf(bitmap<>::required_size(num_slots), 0);
    bitmap<> bm;
    bm.init(buf.data(), num_slots);
    for (auto _ : state)
    {
        size_t slots[64];
        size_t n = bm.alloc_bits_batch(batch, slots);
        if (n > 0)
        {
            std::sort(slots, slots + n);
            bm.free_bits_batch(slots, n);
        }
    }
    state.SetItemsProcessed(state.iterations() * batch * 2);
}
BENCHMARK(BM_Bitmap_BatchChurn)->ThreadRange(1, 16);

static void BM_Bitmap_LimitedAlloc(benchmark::State& state)
{
    const size_t num_slots = 65536;
    const size_t limit = num_slots / 2;
    std::vector<uint8_t> buf(bitmap<>::required_size(num_slots), 0);
    bitmap<> bm;
    bm.init(buf.data(), num_slots);
    for (auto _ : state)
    {
        size_t s = bm.alloc_bit(limit);
        if (s != static_cast<size_t>(-1))
            bm.free_bit(s);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Bitmap_LimitedAlloc);
