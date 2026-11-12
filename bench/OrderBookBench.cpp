// Throughput micro-benchmarks. Build in Release:
//   cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release -j
//   ./build-release/orderbook_bench

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <memory>
#include <random>
#include <vector>

#include "Workload.h"
#include "orderbook/OrderBook.h"

using namespace orderbook;
using namespace orderbook::bench;

// N non-crossing adds spread over 100 price levels per side.
static void BM_AddResting(benchmark::State& state) {
    const auto n = static_cast<OrderId>(state.range(0));
    std::unique_ptr<OrderBook> holder;
    for (auto _ : state) {
        state.PauseTiming();
        holder.reset();  // destroy the previous book outside the timed region
        holder = std::make_unique<OrderBook>();
        OrderBook& book = *holder;
        state.ResumeTiming();
        for (OrderId id = 1; id <= n; ++id) {
            const bool buy = (id & 1) != 0;
            const Price level = static_cast<Price>(id % 100);
            (void)book.AddOrder(Order{OrderType::GoodTillCancel, id, buy ? Side::Buy : Side::Sell,
                                      10, buy ? 9'999 - level : 10'001 + level});
        }
        benchmark::DoNotOptimize(book.Size());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_AddResting)->Arg(10'000)->Arg(100'000)->Unit(benchmark::kMicrosecond);

// Cancel every order of a resting book, in random order.
static void BM_CancelResting(benchmark::State& state) {
    const int perLevel = static_cast<int>(state.range(0)) / 200;  // 100 levels x 2 sides
    std::vector<OrderId> ids(static_cast<std::size_t>(perLevel) * 200);
    std::iota(ids.begin(), ids.end(), OrderId{1});
    std::shuffle(ids.begin(), ids.end(), std::mt19937_64{7});

    std::unique_ptr<OrderBook> holder;
    for (auto _ : state) {
        state.PauseTiming();
        holder.reset();
        holder = std::make_unique<OrderBook>();
        OrderBook& book = *holder;
        BuildRestingBook(book, 100, perLevel);
        state.ResumeTiming();
        for (const OrderId id : ids)
            (void)book.CancelOrder(id);
        benchmark::DoNotOptimize(book.Size());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(ids.size()));
}
BENCHMARK(BM_CancelResting)->Arg(10'000)->Arg(100'000)->Unit(benchmark::kMicrosecond);

// One market order sweeping `levels` x `perLevel` resting asks (one trade per resting order).
static void BM_MarketSweep(benchmark::State& state) {
    const int levels = static_cast<int>(state.range(0));
    const int perLevel = static_cast<int>(state.range(1));
    const auto fills = static_cast<Quantity>(levels * perLevel);

    std::unique_ptr<OrderBook> holder;
    for (auto _ : state) {
        state.PauseTiming();
        holder.reset();
        holder = std::make_unique<OrderBook>();
        OrderBook& book = *holder;
        BuildRestingBook(book, levels, perLevel);
        state.ResumeTiming();
        auto result = book.AddOrder(Order{1'000'000'000, Side::Buy, fills * 10});
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations() * fills);  // items = trades
}
BENCHMARK(BM_MarketSweep)->Args({10, 100})->Args({100, 10})->Unit(benchmark::kMicrosecond);

// Depth snapshot of a deep book.
static void BM_GetOrderInfos(benchmark::State& state) {
    OrderBook book;
    BuildRestingBook(book, static_cast<int>(state.range(0)), static_cast<int>(state.range(1)));
    for (auto _ : state) {
        auto infos = book.GetOrderInfos();
        benchmark::DoNotOptimize(infos);
    }
}
BENCHMARK(BM_GetOrderInfos)->Args({100, 10})->Args({100, 100})->Unit(benchmark::kMicrosecond);

// FillOrKill that is rejected after scanning all liquidity within its limit.
static void BM_FillOrKillReject(benchmark::State& state) {
    OrderBook book;
    BuildRestingBook(book, static_cast<int>(state.range(0)), static_cast<int>(state.range(1)));
    const Quantity tooMuch = 1'000'000'000;
    for (auto _ : state) {
        auto result = book.AddOrder(Order{OrderType::FillOrKill, 1'000'000'000, Side::Buy, tooMuch, 20'000});
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_FillOrKillReject)->Args({100, 10})->Args({100, 100})->Unit(benchmark::kMicrosecond);

// Realistic mixed flow (adds / cancels / aggressive / market / modify).
static void BM_MixedWorkload(benchmark::State& state) {
    const auto ops = GenerateMixedWorkload(static_cast<std::size_t>(state.range(0)));
    std::unique_ptr<OrderBook> holder;
    for (auto _ : state) {
        state.PauseTiming();
        holder.reset();
        holder = std::make_unique<OrderBook>();
        OrderBook& book = *holder;
        state.ResumeTiming();
        for (const Op& op : ops)
            Apply(book, op);
        benchmark::DoNotOptimize(book.Size());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(ops.size()));
}
BENCHMARK(BM_MixedWorkload)->Arg(1'000'000)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
