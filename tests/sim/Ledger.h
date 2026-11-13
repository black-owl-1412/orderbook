#pragma once

// Invariant checker that is independent of the reference model: it rebuilds the
// set of live orders purely from the ENGINE's own outputs (accept/reject, trades)
// plus the definition of each order type, and checks after every event:
//
//   * book never crossed (best bid < best ask)
//   * price-time priority: each trade hits the best price level, and within it the
//     earliest live order
//   * trade price == resting (maker) order's price; both legs agree
//   * no overfill, no zero-quantity trades, no zero-quantity levels
//   * conservation: added == resting + 2 * traded + cancelled
//     (each trade removes its quantity from BOTH the aggressor and the maker)
//   * rejects are justified (e.g. CannotMatch only when nothing crosses)

#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

#include "orderbook/OrderBook.h"
#include "orderbook/ToString.h"

namespace sim {

using namespace ::orderbook;

class Ledger {
public:
    using Violation = std::optional<std::string>;

    Violation OnAdd(OrderType type, OrderId id, Side side, Price price, Quantity qty,
                    const AddResult& result) {
        const Price limit = type == OrderType::Market ? MarketPrice(side) : price;
        const bool canCross = Crosses(side, limit, BestOpposite(side));

        if (!result) {
            switch (result.error()) {
                case RejectReason::ZeroQuantity:
                    if (qty == 0) return std::nullopt;
                    break;
                case RejectReason::DuplicateOrderId:
                    if (live_.contains(id)) return std::nullopt;
                    break;
                case RejectReason::CannotMatch:
                    if ((type == OrderType::FillAndKill || type == OrderType::Market) && !canCross)
                        return std::nullopt;
                    break;
                case RejectReason::CannotFullyFill:
                    if (type == OrderType::FillOrKill && AvailableAgainst(side, limit) < qty)
                        return std::nullopt;
                    break;
                default:
                    break;
            }
            return std::format("unjustified reject {} of {} #{} {} {} @ {}",
                               ToString(result.error()), ToString(type), id, ToString(side), qty,
                               price);
        }

        if (qty == 0)
            return std::format("accepted zero-quantity order #{}", id);
        if ((type == OrderType::FillAndKill || type == OrderType::Market) && !canCross)
            return std::format("{} #{} accepted although nothing crosses", ToString(type), id);

        added_ += qty;
        Quantity remaining = qty;
        for (const Trade& t : *result) {
            const TradeInfo& bid = t.GetBidTrade();
            const TradeInfo& ask = t.GetAskTrade();
            if (bid.price_ != ask.price_ || bid.quantity_ != ask.quantity_)
                return std::format("trade legs disagree for aggressor #{}", id);
            const OrderId aggressorLeg = side == Side::Buy ? bid.orderId_ : ask.orderId_;
            const OrderId makerId = side == Side::Buy ? ask.orderId_ : bid.orderId_;
            if (aggressorLeg != id)
                return std::format("trade aggressor leg is #{}, expected #{}", aggressorLeg, id);
            const Quantity q = bid.quantity_;
            if (q == 0)
                return std::format("zero-quantity trade for #{}", id);

            auto it = live_.find(makerId);
            if (it == live_.end() || it->second.side == side)
                return std::format("maker #{} is not a live opposite order", makerId);
            Live& maker = it->second;
            if (bid.price_ != maker.price)
                return std::format("trade price {} != resting price {} of maker #{}", bid.price_,
                                   maker.price, makerId);
            if (const auto best = BestOpposite(side); !best || *best != maker.price)
                return std::format("price priority: traded at {} but best opposite is {}",
                                   maker.price, best ? std::to_string(*best) : "none");
            const OrderId front = Queues(maker.side).at(maker.price).begin()->second;
            if (front != makerId)
                return std::format("time priority: at {} traded #{} before earlier #{}",
                                   maker.price, makerId, front);
            if (!Crosses(side, limit, maker.price))
                return std::format("#{} traded through its limit {} at {}", id, limit, maker.price);
            if (q > maker.remaining || q > remaining)
                return std::format("overfill: trade {} vs maker {} / aggressor {}", q,
                                   maker.remaining, remaining);

            maker.remaining -= q;
            remaining -= q;
            traded_ += q;
            if (maker.remaining == 0)
                Remove(makerId);
        }

        if (remaining > 0) {
            if (Crosses(side, limit, BestOpposite(side)))
                return std::format("#{} stopped matching with {} left while the book still crosses",
                                   id, remaining);
            if (type == OrderType::FillOrKill)
                return std::format("FillOrKill #{} accepted but left {} unfilled", id, remaining);
            if (IsImmediateOrCancel(type))
                cancelled_ += remaining;
            else
                Insert(id, side, price, remaining);
        }
        return std::nullopt;
    }

    Violation OnCancel(OrderId id, const CancelResult& result) {
        auto it = live_.find(id);
        if (it == live_.end()) {
            if (result)
                return std::format("cancel of non-live #{} succeeded", id);
            if (result.error() != RejectReason::UnknownOrderId)
                return std::format("cancel #{} rejected with {}", id, ToString(result.error()));
            return std::nullopt;
        }
        if (!result)
            return std::format("cancel of live #{} rejected with {}", id, ToString(result.error()));
        cancelled_ += it->second.remaining;
        Remove(id);
        return std::nullopt;
    }

    // Cheap, after every event.
    Violation CheckTop(const OrderBook& book) const {
        if (book.Size() != live_.size())
            return std::format("engine has {} orders, ledger {}", book.Size(), live_.size());
        const auto bid = book.BestBid();
        const auto ask = book.BestAsk();
        if (bid != BestOf(Side::Buy) || ask != BestOf(Side::Sell))
            return std::format("best prices differ from ledger");
        if (bid && ask && *bid >= *ask)
            return std::format("book crossed: bid {} >= ask {}", *bid, *ask);
        return std::nullopt;
    }

    // O(levels): zero-quantity levels + conservation using the engine's own depth.
    Violation CheckLevels(const OrderBook& book) const {
        const auto infos = book.GetOrderInfos();
        std::uint64_t resting = 0;
        for (const LevelInfos* side : {&infos.GetBids(), &infos.GetAsks()})
            for (const LevelInfo& l : *side) {
                if (l.quantity_ == 0)
                    return std::format("zero-quantity level at {}", l.price_);
                resting += l.quantity_;
            }
        if (added_ != resting + 2 * traded_ + cancelled_)
            return std::format("conservation: added {} != resting {} + 2*traded {} + cancelled {}",
                               added_, resting, traded_, cancelled_);
        return std::nullopt;
    }

    // O(orders): every engine level total equals the ledger's.
    Violation CheckLevelTotals(const OrderBook& book) const {
        const auto infos = book.GetOrderInfos();
        auto compare = [&](Side side, const LevelInfos& levels) -> Violation {
            const auto& queues = Queues(side);
            if (levels.size() != queues.size())
                return std::format("{} level count {} != ledger {}", ToString(side), levels.size(),
                                   queues.size());
            for (const LevelInfo& l : levels) {
                auto q = queues.find(l.price_);
                if (q == queues.end())
                    return std::format("engine level {} unknown to ledger", l.price_);
                std::uint64_t sum = 0;
                for (const auto& [seq, oid] : q->second)
                    sum += live_.at(oid).remaining;
                if (sum != l.quantity_)
                    return std::format("level {} quantity {} != ledger {}", l.price_, l.quantity_,
                                       sum);
            }
            return std::nullopt;
        };
        if (auto v = compare(Side::Buy, infos.GetBids())) return v;
        return compare(Side::Sell, infos.GetAsks());
    }

    std::uint64_t Added() const { return added_; }
    std::uint64_t Traded() const { return traded_; }
    std::uint64_t Cancelled() const { return cancelled_; }

    // Longest FIFO queue currently at any price level.
    std::size_t LongestQueue() const {
        std::size_t longest = 0;
        for (const auto* side : {&bidQueues_, &askQueues_})
            for (const auto& [price, q] : *side)
                longest = std::max(longest, q.size());
        return longest;
    }

private:
    struct Live {
        Side side;
        Price price;
        Quantity remaining;
        std::uint64_t seq;
    };
    using Queue = std::map<std::uint64_t, OrderId>;  // seq -> id, i.e. FIFO order

    static bool Crosses(Side aggressor, Price limit, std::optional<Price> resting) {
        if (!resting)
            return false;
        return aggressor == Side::Buy ? limit >= *resting : limit <= *resting;
    }

    std::map<Price, Queue>& Queues(Side s) { return s == Side::Buy ? bidQueues_ : askQueues_; }
    const std::map<Price, Queue>& Queues(Side s) const {
        return s == Side::Buy ? bidQueues_ : askQueues_;
    }

    std::optional<Price> BestOf(Side s) const {
        const auto& q = Queues(s);
        if (q.empty())
            return std::nullopt;
        return s == Side::Buy ? q.rbegin()->first : q.begin()->first;
    }
    std::optional<Price> BestOpposite(Side aggressor) const {
        return BestOf(aggressor == Side::Buy ? Side::Sell : Side::Buy);
    }

    std::uint64_t AvailableAgainst(Side aggressor, Price limit) const {
        std::uint64_t sum = 0;
        for (const auto& [id, o] : live_)
            if (o.side != aggressor && Crosses(aggressor, limit, o.price))
                sum += o.remaining;
        return sum;
    }

    void Insert(OrderId id, Side side, Price price, Quantity qty) {
        const std::uint64_t seq = nextSeq_++;
        live_.emplace(id, Live{side, price, qty, seq});
        Queues(side)[price].emplace(seq, id);
    }

    void Remove(OrderId id) {
        const Live o = live_.at(id);
        auto& queues = Queues(o.side);
        auto level = queues.find(o.price);
        level->second.erase(o.seq);
        if (level->second.empty())
            queues.erase(level);
        live_.erase(id);
    }

    std::unordered_map<OrderId, Live> live_;
    std::map<Price, Queue> bidQueues_;
    std::map<Price, Queue> askQueues_;
    std::uint64_t nextSeq_ = 0;
    std::uint64_t added_ = 0;
    std::uint64_t traded_ = 0;
    std::uint64_t cancelled_ = 0;
};

}  // namespace sim
