#include "orderbook/OrderBook.h"

#include <algorithm>

namespace orderbook {

namespace {

// Walks levels best-first while `acceptable(price)` holds and reports whether
// at least `needed` quantity is resting there. O(levels inspected), thanks to
// the per-level running totals.
template <typename Levels, typename Acceptable>
bool HasLiquidity(const Levels& levels, Acceptable acceptable, Quantity needed) {
    for (auto it = levels.rbegin(); it != levels.rend(); ++it) {  // best level is last
        const auto& [price, level] = *it;
        if (!acceptable(price))
            break;
        if (level.quantity >= needed)
            return true;
        needed -= level.quantity;
    }
    return false;
}

}  // namespace

bool OrderBook::CanMatch(Side side, Price price) const {
    // A buy matches against the best ASK; a sell against the best BID.
    if (side == Side::Buy)
        return !asks_.empty() && price >= *BestAsk();
    return !bids_.empty() && price <= *BestBid();
}

bool OrderBook::CanFullyFill(Side side, Price price, Quantity quantity) const {
    if (side == Side::Buy)
        return HasLiquidity(asks_, [price](Price ask) { return ask <= price; }, quantity);
    return HasLiquidity(bids_, [price](Price bid) { return bid >= price; }, quantity);
}

// Trades the taker against the opposite side, best price first and oldest order
// first within a price, until it is filled or its limit no longer crosses.
// Every trade executes at the resting (maker) order's price.
template <typename Levels>
void OrderBook::Match(Levels& opposite, Order& taker, Trades& trades) {
    const bool takerBuys = taker.GetSide() == Side::Buy;

    while (!taker.IsFilled() && !opposite.empty()) {
        auto best = opposite.end();  // best level is last
        --best;  // not std::prev: flat_map iterators are not legacy bidirectional iterators
        const Price price = (*best).first;
        if (takerBuys ? taker.GetPrice() < price : taker.GetPrice() > price)
            break;

        Level& level = (*best).second;
        while (!taker.IsFilled() && !level.Empty()) {
            OrderNode* maker = level.head;
            const Quantity quantity =
                std::min(taker.GetRemainingQuantity(), maker->order.GetRemainingQuantity());
            taker.Fill(quantity);
            maker->order.Fill(quantity);
            level.quantity -= quantity;

            const TradeInfo takerLeg{taker.GetOrderId(), price, quantity};
            const TradeInfo makerLeg{maker->order.GetOrderId(), price, quantity};
            if (takerBuys)
                trades.emplace_back(takerLeg, makerLeg);
            else
                trades.emplace_back(makerLeg, takerLeg);

            if (maker->order.IsFilled()) {
                level.Unlink(maker);
                orders_.erase(maker->order.GetOrderId());
                pool_.Destroy(maker);
            }
        }

        // Erase the level only after we are done with the level reference. Erasing
        // the last element is O(1) for both std::map and std::flat_map.
        if (level.Empty())
            opposite.erase(best);
    }
}

AddResult OrderBook::AddOrder(const OrderPointer& order) {
    if (!order)
        return std::unexpected(RejectReason::InvalidOrder);
    return AddOrder(*order);
}

AddResult OrderBook::AddOrder(const Order& order) {
    if (order.GetRemainingQuantity() == 0)
        return std::unexpected(RejectReason::ZeroQuantity);
    if (orders_.contains(order.GetOrderId()))
        return std::unexpected(RejectReason::DuplicateOrderId);

    switch (order.GetOrderType()) {
        case OrderType::FillAndKill:
        case OrderType::Market:
            if (!CanMatch(order.GetSide(), order.GetPrice()))
                return std::unexpected(RejectReason::CannotMatch);
            break;
        case OrderType::FillOrKill:
            // Check BEFORE touching the book: a FOK must be all-or-nothing.
            if (!CanFullyFill(order.GetSide(), order.GetPrice(), order.GetRemainingQuantity()))
                return std::unexpected(RejectReason::CannotFullyFill);
            break;
        case OrderType::GoodTillCancel:
        case OrderType::GoodForDay:
            break;
    }

    // Match first, then rest only what is left. An order that fills completely
    // (or an immediate-or-cancel remainder) never touches the book's own side:
    // no node, no level, no id-index entry.
    Order taker = order;
    Trades trades;
    if (taker.GetSide() == Side::Buy)
        Match(asks_, taker, trades);
    else
        Match(bids_, taker, trades);

    if (!taker.IsFilled() && !IsImmediateOrCancel(taker.GetOrderType())) {
        OrderNode* node = pool_.Create(taker);
        Level& level =
            taker.GetSide() == Side::Buy ? bids_[taker.GetPrice()] : asks_[taker.GetPrice()];
        level.PushBack(node);
        level.quantity += taker.GetRemainingQuantity();
        orders_.emplace(taker.GetOrderId(), node);
    }
    return trades;
}

CancelResult OrderBook::CancelOrder(OrderId orderId) {
    auto it = orders_.find(orderId);
    if (it == orders_.end())
        return std::unexpected(RejectReason::UnknownOrderId);

    OrderNode* node = it->second;
    orders_.erase(it);

    auto unlinkFrom = [node](auto& levels) {
        auto levelIt = levels.find(node->order.GetPrice());
        Level& level = (*levelIt).second;
        level.quantity -= node->order.GetRemainingQuantity();
        level.Unlink(node);
        if (level.Empty())
            levels.erase(levelIt);
    };
    if (node->order.GetSide() == Side::Buy)
        unlinkFrom(bids_);
    else
        unlinkFrom(asks_);
    pool_.Destroy(node);
    return {};
}

AddResult OrderBook::ModifyOrder(const OrderModify& order) {
    auto it = orders_.find(order.GetOrderId());
    if (it == orders_.end())
        return std::unexpected(RejectReason::UnknownOrderId);
    // Validate before cancelling, otherwise a bad modify would silently delete the order.
    if (order.GetQuantity() == 0)
        return std::unexpected(RejectReason::ZeroQuantity);

    // Only GTC/GFD orders can be resting, so re-adding cannot hit the
    // FAK/FOK/Market liquidity checks or a duplicate id.
    const OrderType type = it->second->order.GetOrderType();  // copy before cancel
    (void)CancelOrder(order.GetOrderId());
    return AddOrder(order.ToOrder(type));
}

std::vector<OrderId> OrderBook::CancelGoodForDayOrders() {
    // Collect first: cancelling while iterating orders_ would invalidate the iterator.
    std::vector<OrderId> ids;
    for (const auto& [id, node] : orders_)
        if (node->order.GetOrderType() == OrderType::GoodForDay)
            ids.push_back(id);
    std::ranges::sort(ids);  // unordered_map order is unspecified; make output deterministic

    for (const OrderId id : ids)
        (void)CancelOrder(id);
    return ids;
}

OrderBookLevelInfos OrderBook::GetOrderInfos() const {
    LevelInfos bidInfos;
    LevelInfos askInfos;
    bidInfos.reserve(bids_.size());
    askInfos.reserve(asks_.size());

    // Levels are stored best-last, snapshots are reported best-first.
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it)
        bidInfos.push_back(LevelInfo{(*it).first, (*it).second.quantity});
    for (auto it = asks_.rbegin(); it != asks_.rend(); ++it)
        askInfos.push_back(LevelInfo{(*it).first, (*it).second.quantity});

    return OrderBookLevelInfos{std::move(bidInfos), std::move(askInfos)};
}

}  // namespace orderbook
