// Differential ("model-based") testing: drive the real OrderBook and a tiny,
// obviously-correct reference implementation with the same random operations
// and require identical results, trades and depth after every single step.
//
// The reference (tests/sim/ReferenceBook.h) makes no attempt to be fast; it exists
// so that performance refactors of the real engine can be checked mechanically.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <vector>

#include "orderbook/OrderBook.h"
#include "orderbook/ToString.h"
#include "sim/ReferenceBook.h"

using namespace orderbook;

namespace {

using sim::ReferenceBook;  // shared oracle, see tests/sim/ReferenceBook.h

void ExpectSameAdd(const AddResult& real, const AddResult& ref, std::size_t step) {
    ASSERT_EQ(real.has_value(), ref.has_value())
        << "step " << step << ": real "
        << (real ? "accepted" : ToString(real.error())) << ", reference "
        << (ref ? "accepted" : ToString(ref.error()));
    if (!real) {
        ASSERT_EQ(real.error(), ref.error()) << "step " << step;
        return;
    }
    ASSERT_EQ(real->size(), ref->size()) << "step " << step << ": trade count";
    for (std::size_t i = 0; i < real->size(); ++i) {
        const Trade& a = (*real)[i];
        const Trade& b = (*ref)[i];
        ASSERT_EQ(a.GetBidTrade().orderId_, b.GetBidTrade().orderId_) << "step " << step;
        ASSERT_EQ(a.GetAskTrade().orderId_, b.GetAskTrade().orderId_) << "step " << step;
        ASSERT_EQ(a.GetPrice(), b.GetPrice()) << "step " << step;
        ASSERT_EQ(a.GetQuantity(), b.GetQuantity()) << "step " << step;
    }
}

void ExpectSameDepth(const OrderBook& real, const ReferenceBook& ref, std::size_t step) {
    ASSERT_EQ(real.Size(), ref.Size()) << "step " << step;
    const auto a = real.GetOrderInfos();
    const auto b = ref.Depth();
    auto same = [&](const LevelInfos& x, const LevelInfos& y, const char* side) {
        ASSERT_EQ(x.size(), y.size()) << "step " << step << " " << side << " levels";
        for (std::size_t i = 0; i < x.size(); ++i) {
            ASSERT_EQ(x[i].price_, y[i].price_) << "step " << step << " " << side;
            ASSERT_EQ(x[i].quantity_, y[i].quantity_) << "step " << step << " " << side;
        }
    };
    same(a.GetBids(), b.GetBids(), "bid");
    same(a.GetAsks(), b.GetAsks(), "ask");
}

void RunRandomSession(std::uint64_t seed, std::size_t steps) {
    std::mt19937_64 rng{seed};
    std::uniform_int_distribution<int> pct{0, 99};
    std::uniform_int_distribution<Price> price{95, 105};  // narrow range => lots of crossing
    std::uniform_int_distribution<Quantity> qty{0, 20};   // includes 0 to exercise rejects
    std::bernoulli_distribution coin{0.5};
    constexpr std::array kTypes{OrderType::GoodTillCancel, OrderType::GoodTillCancel,
                                OrderType::GoodForDay,     OrderType::FillAndKill,
                                OrderType::FillOrKill,     OrderType::Market};
    std::uniform_int_distribution<std::size_t> typeIdx{0, kTypes.size() - 1};

    OrderBook real;
    ReferenceBook ref;
    OrderId nextId = 1;
    std::uniform_int_distribution<OrderId> anyId{1, 1};

    for (std::size_t step = 0; step < steps; ++step) {
        const int roll = pct(rng);
        const Side side = coin(rng) ? Side::Buy : Side::Sell;
        anyId = std::uniform_int_distribution<OrderId>{1, nextId};  // may hit dead/unknown ids

        if (roll < 60) {
            const OrderType type = kTypes[typeIdx(rng)];
            // 2% chance to reuse an existing id to exercise DuplicateOrderId.
            const OrderId id = pct(rng) < 2 ? anyId(rng) : nextId++;
            const Price p = price(rng);
            Quantity q = qty(rng);
            // Boundary cases random sizes almost never hit: a FillOrKill for EXACTLY the
            // available quantity (must fill) or one more (must reject).
            if (type == OrderType::FillOrKill && coin(rng)) {
                const Quantity available = ref.CrossingQuantity(side, p, Quantity{1'000'000});
                if (available > 0)
                    q = available + (coin(rng) ? 0 : 1);
            }
            ExpectSameAdd(real.AddOrder(Order{type, id, side, q, p}), ref.Add(type, id, side, p, q),
                          step);
        } else if (roll < 85) {
            const OrderId id = anyId(rng);
            const auto a = real.CancelOrder(id);
            const auto b = ref.Cancel(id);
            ASSERT_EQ(a.has_value(), b.has_value()) << "step " << step << " cancel " << id;
        } else if (roll < 99) {
            const OrderId id = anyId(rng);
            const Price p = price(rng);
            const Quantity q = qty(rng);
            ExpectSameAdd(real.ModifyOrder(OrderModify{id, side, q, p}), ref.Modify(id, side, p, q),
                          step);
        } else {
            ASSERT_EQ(real.CancelGoodForDayOrders(), ref.EndOfDay()) << "step " << step;
        }
        ExpectSameDepth(real, ref, step);
        if (::testing::Test::HasFatalFailure())
            return;
    }
}

}  // namespace

class Differential : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(Differential, MatchesReferenceModel) {
    RunRandomSession(GetParam(), 5'000);
}

INSTANTIATE_TEST_SUITE_P(Seeds, Differential, ::testing::Range<std::uint64_t>(1, 21));
