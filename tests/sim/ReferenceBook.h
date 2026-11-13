#pragma once

// Slow but obviously-correct order book used as the oracle for equivalence tests.
//
// Every resting order lives in ONE std::map per side keyed by
// (price priority, arrival sequence), so map.begin() is always the order that
// must trade next. No price levels, no iterators stored, no cleverness.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include "orderbook/OrderBook.h"

namespace sim {

using namespace ::orderbook;

class ReferenceBook {
public:
    AddResult Add(OrderType type, OrderId id, Side side, Price price, Quantity qty) {
        if (qty == 0)
            return std::unexpected(RejectReason::ZeroQuantity);
        if (index_.contains(id))
            return std::unexpected(RejectReason::DuplicateOrderId);
        if (type == OrderType::Market)
            price = MarketPrice(side);

        if ((type == OrderType::FillAndKill || type == OrderType::Market) &&
            CrossingQuantity(side, price, 1) == 0)
            return std::unexpected(RejectReason::CannotMatch);
        if (type == OrderType::FillOrKill && CrossingQuantity(side, price, qty) < qty)
            return std::unexpected(RejectReason::CannotFullyFill);

        Trades trades;
        Quantity remaining = qty;
        Side_& opposite = side == Side::Buy ? asks_ : bids_;
        while (remaining > 0 && !opposite.empty()) {
            auto it = opposite.begin();
            Resting& maker = it->second;
            if (!Crosses(side, price, maker.price))
                break;
            const Quantity q = std::min(remaining, maker.remaining);
            remaining -= q;
            maker.remaining -= q;
            const OrderId bidId = side == Side::Buy ? id : maker.id;
            const OrderId askId = side == Side::Buy ? maker.id : id;
            trades.emplace_back(TradeInfo{bidId, maker.price, q}, TradeInfo{askId, maker.price, q});
            if (maker.remaining == 0) {
                Unindex(maker.id);
                opposite.erase(it);
            }
        }

        if (remaining > 0 && !IsImmediateOrCancel(type))
            Insert(Resting{id, type, side, price, remaining, nextSeq_++});
        return trades;
    }

    CancelResult Cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end())
            return std::unexpected(RejectReason::UnknownOrderId);
        const Location loc = it->second;
        (loc.side == Side::Buy ? bids_ : asks_).erase(loc.key);
        Unindex(id);
        return {};
    }

    AddResult Modify(OrderId id, Side side, Price price, Quantity qty) {
        auto it = index_.find(id);
        if (it == index_.end())
            return std::unexpected(RejectReason::UnknownOrderId);
        if (qty == 0)
            return std::unexpected(RejectReason::ZeroQuantity);
        const Location loc = it->second;
        const OrderType type = (loc.side == Side::Buy ? bids_ : asks_).at(loc.key).type;
        (void)Cancel(id);
        return Add(type, id, side, price, qty);
    }

    std::vector<OrderId> EndOfDay() {
        std::vector<OrderId> ids;
        for (const Side_* book : {&bids_, &asks_})
            for (const auto& [key, r] : *book)
                if (r.type == OrderType::GoodForDay)
                    ids.push_back(r.id);
        std::ranges::sort(ids);
        for (OrderId id : ids)
            (void)Cancel(id);
        return ids;
    }

    std::size_t Size() const { return index_.size(); }
    bool IsLive(OrderId id) const { return index_.contains(id); }

    std::optional<Price> BestBid() const {
        if (bids_.empty())
            return std::nullopt;
        return bids_.begin()->second.price;
    }
    std::optional<Price> BestAsk() const {
        if (asks_.empty())
            return std::nullopt;
        return asks_.begin()->second.price;
    }

    // Aggregated depth, best level first, at most `maxLevels` levels per side.
    OrderBookLevelInfos Depth(std::size_t maxLevels = SIZE_MAX) const {
        return {Aggregate(bids_, maxLevels), Aggregate(asks_, maxLevels)};
    }

    // Quantity an aggressor (side, limit price) could trade right now, stopping early at `cap`.
    Quantity CrossingQuantity(Side side, Price price, Quantity cap) const {
        const Side_& opposite = side == Side::Buy ? asks_ : bids_;
        std::uint64_t sum = 0;
        for (const auto& [key, r] : opposite) {
            if (!Crosses(side, price, r.price) || sum >= cap)
                break;
            sum += r.remaining;
        }
        return static_cast<Quantity>(std::min<std::uint64_t>(sum, cap));
    }

    // Total quantity resting in the best `levels` price levels of `side`.
    std::uint64_t VisibleQuantity(Side side, std::size_t levels) const {
        std::uint64_t sum = 0;
        for (const auto& l : Aggregate(side == Side::Buy ? bids_ : asks_, levels))
            sum += l.quantity_;
        return sum;
    }

    template <typename Rng>
    std::optional<OrderId> RandomLiveId(Rng& rng) const {
        if (live_.empty())
            return std::nullopt;
        std::uniform_int_distribution<std::size_t> pick{0, live_.size() - 1};
        return live_[pick(rng)];
    }

private:
    struct Resting {
        OrderId id;
        OrderType type;
        Side side;
        Price price;
        Quantity remaining;
        std::uint64_t seq;  // arrival order => time priority
    };
    // (price key, seq): bids use -price so that begin() is the HIGHEST bid.
    using Key = std::pair<std::int64_t, std::uint64_t>;
    using Side_ = std::map<Key, Resting>;
    struct Location {
        Side side;
        Key key;
        std::size_t livePos;
    };

    static bool Crosses(Side aggressor, Price limit, Price resting) {
        return aggressor == Side::Buy ? limit >= resting : limit <= resting;
    }

    void Insert(const Resting& r) {
        const Key key{r.side == Side::Buy ? -std::int64_t{r.price} : std::int64_t{r.price}, r.seq};
        (r.side == Side::Buy ? bids_ : asks_).emplace(key, r);
        index_.emplace(r.id, Location{r.side, key, live_.size()});
        live_.push_back(r.id);
    }

    // Remove from the id index and the random-access live list (swap-remove).
    void Unindex(OrderId id) {
        const std::size_t pos = index_.at(id).livePos;
        const OrderId moved = live_.back();
        live_[pos] = moved;
        index_.at(moved).livePos = pos;
        live_.pop_back();
        index_.erase(id);
    }

    static LevelInfos Aggregate(const Side_& book, std::size_t maxLevels) {
        LevelInfos out;
        for (const auto& [key, r] : book) {
            if (!out.empty() && out.back().price_ == r.price)
                out.back().quantity_ += r.remaining;
            else if (out.size() == maxLevels)
                break;
            else
                out.push_back({r.price, r.remaining});
        }
        return out;
    }

    Side_ bids_;
    Side_ asks_;
    std::unordered_map<OrderId, Location> index_;
    std::vector<OrderId> live_;
    std::uint64_t nextSeq_ = 0;
};

}  // namespace sim
