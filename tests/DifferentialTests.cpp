// Differential ("model-based") testing: drive the real OrderBook and a tiny,
// obviously-correct reference implementation with the same random operations
// and require identical results, trades and depth after every single step.
//
// The reference is O(n) per operation and makes no attempt to be fast; it exists
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

using namespace orderbook;

namespace {

class ReferenceBook {
public:
    AddResult Add(OrderType type, OrderId id, Side side, Price price, Quantity qty) {
        if (qty == 0)
            return std::unexpected(RejectReason::ZeroQuantity);
        if (Find(id))
            return std::unexpected(RejectReason::DuplicateOrderId);
        if (type == OrderType::Market)
            price = MarketPrice(side);

        if (type == OrderType::FillAndKill || type == OrderType::Market) {
            if (!BestCrossing(side, price))
                return std::unexpected(RejectReason::CannotMatch);
        } else if (type == OrderType::FillOrKill) {
            std::uint64_t available = 0;
            for (const auto& r : resting_)
                if (r.side != side && Crosses(side, price, r.price))
                    available += r.remaining;
            if (available < qty)
                return std::unexpected(RejectReason::CannotFullyFill);
        }

        Trades trades;
        Quantity remaining = qty;
        while (remaining > 0) {
            auto best = BestCrossing(side, price);
            if (!best)
                break;
            Resting& maker = resting_[*best];
            const Quantity q = std::min(remaining, maker.remaining);
            remaining -= q;
            maker.remaining -= q;
            const OrderId bidId = side == Side::Buy ? id : maker.id;
            const OrderId askId = side == Side::Buy ? maker.id : id;
            trades.emplace_back(TradeInfo{bidId, maker.price, q}, TradeInfo{askId, maker.price, q});
            if (maker.remaining == 0)
                resting_.erase(resting_.begin() + static_cast<std::ptrdiff_t>(*best));
        }

        if (remaining > 0 && !IsImmediateOrCancel(type))
            resting_.push_back({id, type, side, price, remaining, nextSeq_++});
        return trades;
    }

    CancelResult Cancel(OrderId id) {
        auto idx = Find(id);
        if (!idx)
            return std::unexpected(RejectReason::UnknownOrderId);
        resting_.erase(resting_.begin() + static_cast<std::ptrdiff_t>(*idx));
        return {};
    }

    AddResult Modify(OrderId id, Side side, Price price, Quantity qty) {
        auto idx = Find(id);
        if (!idx)
            return std::unexpected(RejectReason::UnknownOrderId);
        if (qty == 0)
            return std::unexpected(RejectReason::ZeroQuantity);
        const OrderType type = resting_[*idx].type;
        (void)Cancel(id);
        return Add(type, id, side, price, qty);
    }

    std::vector<OrderId> EndOfDay() {
        std::vector<OrderId> ids;
        for (const auto& r : resting_)
            if (r.type == OrderType::GoodForDay)
                ids.push_back(r.id);
        std::ranges::sort(ids);
        std::erase_if(resting_, [](const Resting& r) { return r.type == OrderType::GoodForDay; });
        return ids;
    }

    std::size_t Size() const { return resting_.size(); }

    OrderBookLevelInfos Depth() const {
        std::map<Price, Quantity, std::greater<Price>> bids;
        std::map<Price, Quantity> asks;
        for (const auto& r : resting_)
            (r.side == Side::Buy ? bids[r.price] : asks[r.price]) += r.remaining;
        LevelInfos b, a;
        for (auto [p, q] : bids) b.push_back({p, q});
        for (auto [p, q] : asks) a.push_back({p, q});
        return {b, a};
    }

private:
    struct Resting {
        OrderId id;
        OrderType type;
        Side side;
        Price price;
        Quantity remaining;
        std::uint64_t seq;  // arrival order for time priority
    };

    static bool Crosses(Side aggressor, Price aggressorPrice, Price restingPrice) {
        return aggressor == Side::Buy ? aggressorPrice >= restingPrice
                                      : aggressorPrice <= restingPrice;
    }

    // Index of the best resting order on the opposite side that crosses, if any.
    std::optional<std::size_t> BestCrossing(Side side, Price price) const {
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < resting_.size(); ++i) {
            const auto& r = resting_[i];
            if (r.side == side || !Crosses(side, price, r.price))
                continue;
            if (!best) {
                best = i;
                continue;
            }
            const auto& b = resting_[*best];
            const bool betterPrice = side == Side::Buy ? r.price < b.price : r.price > b.price;
            if (betterPrice || (r.price == b.price && r.seq < b.seq))
                best = i;
        }
        return best;
    }

    std::optional<std::size_t> Find(OrderId id) const {
        for (std::size_t i = 0; i < resting_.size(); ++i)
            if (resting_[i].id == id)
                return i;
        return std::nullopt;
    }

    std::vector<Resting> resting_;
    std::uint64_t nextSeq_ = 0;
};

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
            const Quantity q = qty(rng);
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
