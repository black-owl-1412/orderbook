#pragma once

// Deterministic synthetic order flow shared by the benchmarks and the latency tool.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "orderbook/OrderBook.h"

namespace orderbook::bench {

struct Op {
    enum class Kind : std::uint8_t { Add, Cancel, Modify };
    Kind kind;
    OrderType type;
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;
};

inline void Apply(OrderBook& book, const Op& op) {
    switch (op.kind) {
        case Op::Kind::Add:
            (void)book.AddOrder(Order{op.type, op.id, op.side, op.quantity, op.price});
            break;
        case Op::Kind::Cancel:
            (void)book.CancelOrder(op.id);
            break;
        case Op::Kind::Modify:
            (void)book.ModifyOrder(OrderModify{op.id, op.side, op.quantity, op.price});
            break;
    }
}

// Rough mix of a liquid instrument:
//   50% passive limit adds 1..20 ticks away from mid (these rest)
//   30% cancels of a previously added order (some already filled -> UnknownOrderId)
//   10% aggressive limits 0..2 ticks through mid
//    5% market orders
//    5% modifies (re-price a passive order)
// The mid price random-walks so the book keeps moving.
inline std::vector<Op> GenerateMixedWorkload(std::size_t count, std::uint64_t seed = 42) {
    std::mt19937_64 rng{seed};
    std::uniform_int_distribution<int> pct{0, 99};
    std::uniform_int_distribution<Price> passiveOffset{1, 20};
    std::uniform_int_distribution<Price> aggressiveOffset{0, 2};
    std::uniform_int_distribution<Quantity> qty{1, 100};
    std::bernoulli_distribution coin{0.5};

    std::vector<Op> ops;
    ops.reserve(count);
    std::vector<OrderId> live;  // ids that may still be resting
    live.reserve(count);

    Price mid = 10'000;
    OrderId nextId = 1;

    auto takeLive = [&]() -> OrderId {
        std::uniform_int_distribution<std::size_t> pick{0, live.size() - 1};
        const std::size_t i = pick(rng);
        const OrderId id = live[i];
        live[i] = live.back();
        live.pop_back();
        return id;
    };

    while (ops.size() < count) {
        if (pct(rng) < 2)
            mid += coin(rng) ? 1 : -1;

        const int roll = pct(rng);
        const Side side = coin(rng) ? Side::Buy : Side::Sell;
        const Price passive = side == Side::Buy ? mid - passiveOffset(rng) : mid + passiveOffset(rng);

        if (roll < 50 || live.empty()) {
            const OrderId id = nextId++;
            ops.push_back({Op::Kind::Add, OrderType::GoodTillCancel, id, side, passive, qty(rng)});
            live.push_back(id);
        } else if (roll < 80) {
            ops.push_back({Op::Kind::Cancel, OrderType::GoodTillCancel, takeLive(), side, 0, 0});
        } else if (roll < 90) {
            const Price through =
                side == Side::Buy ? mid + aggressiveOffset(rng) : mid - aggressiveOffset(rng);
            const OrderId id = nextId++;
            ops.push_back({Op::Kind::Add, OrderType::GoodTillCancel, id, side, through, qty(rng)});
            live.push_back(id);
        } else if (roll < 95) {
            ops.push_back({Op::Kind::Add, OrderType::Market, nextId++, side, 0, qty(rng)});
        } else {
            // Side must match the original order's side for a sensible re-price;
            // we don't track it, so a mismatched modify simply re-adds on the other side.
            const OrderId id = takeLive();
            ops.push_back({Op::Kind::Modify, OrderType::GoodTillCancel, id, side, passive, qty(rng)});
            live.push_back(id);
        }
    }
    return ops;
}

// A book with `levels` price levels per side, `perLevel` orders each, best bid
// 9'999 / best ask 10'001. Ids start at `firstId`.
inline void BuildRestingBook(OrderBook& book, int levels, int perLevel, Quantity qty = 10,
                             OrderId firstId = 1) {
    OrderId id = firstId;
    for (int l = 0; l < levels; ++l)
        for (int k = 0; k < perLevel; ++k) {
            (void)book.AddOrder(Order{OrderType::GoodTillCancel, id++, Side::Buy, qty, 9'999 - l});
            (void)book.AddOrder(Order{OrderType::GoodTillCancel, id++, Side::Sell, qty, 10'001 + l});
        }
}

}  // namespace orderbook::bench
