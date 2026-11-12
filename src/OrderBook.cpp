#include "orderbook/OrderBook.h"

#include <algorithm>
#include <iterator>
#include <numeric>

namespace orderbook {

namespace {

// Removes one order from its price level and drops the level if it is now empty.
template <typename Levels>
void EraseFromLevel(Levels& levels, Price price, OrderPointers::iterator location) {
    auto level = levels.find(price);
    level->second.erase(location);
    if (level->second.empty())
        levels.erase(level);
}

// Walks levels best-first while `acceptable(price)` holds and reports whether
// at least `needed` quantity is resting there. O(orders inspected).
template <typename Levels, typename Acceptable>
bool HasLiquidity(const Levels& levels, Acceptable acceptable, Quantity needed) {
    for (const auto& [price, orders] : levels) {
        if (!acceptable(price))
            break;
        for (const auto& order : orders) {
            if (order->GetRemainingQuantity() >= needed)
                return true;
            needed -= order->GetRemainingQuantity();
        }
    }
    return false;
}

}  // namespace

bool OrderBook::CanMatch(Side side, Price price) const {
    // A buy matches against the best ASK; a sell against the best BID.
    // (The original code looked at its own side and could dereference an empty map.)
    if (side == Side::Buy)
        return !asks_.empty() && price >= asks_.begin()->first;
    return !bids_.empty() && price <= bids_.begin()->first;
}

bool OrderBook::CanFullyFill(Side side, Price price, Quantity quantity) const {
    if (side == Side::Buy)
        return HasLiquidity(asks_, [price](Price ask) { return ask <= price; }, quantity);
    return HasLiquidity(bids_, [price](Price bid) { return bid >= price; }, quantity);
}

Trades OrderBook::MatchOrders(Side aggressorSide) {
    Trades trades;

    while (!bids_.empty() && !asks_.empty()) {
        auto bidLevel = bids_.begin();
        auto askLevel = asks_.begin();
        if (bidLevel->first < askLevel->first)
            break;  // book is no longer crossed

        // The book was uncrossed before the incoming order arrived, so the
        // incoming (aggressor) order is the only thing crossing and it trades at
        // the resting (maker) order's price.
        const Price executionPrice =
            aggressorSide == Side::Buy ? askLevel->first : bidLevel->first;

        OrderPointers& bids = bidLevel->second;
        OrderPointers& asks = askLevel->second;

        while (!bids.empty() && !asks.empty()) {
            // Copy the shared_ptrs: front() references would dangle after pop_front().
            OrderPointer bid = bids.front();
            OrderPointer ask = asks.front();

            const Quantity quantity =
                std::min(bid->GetRemainingQuantity(), ask->GetRemainingQuantity());
            bid->Fill(quantity);
            ask->Fill(quantity);

            trades.emplace_back(TradeInfo{bid->GetOrderId(), executionPrice, quantity},
                                TradeInfo{ask->GetOrderId(), executionPrice, quantity});

            if (bid->IsFilled()) {
                bids.pop_front();
                orders_.erase(bid->GetOrderId());
            }
            if (ask->IsFilled()) {
                asks.pop_front();
                orders_.erase(ask->GetOrderId());
            }
        }

        // Erase empty levels only after we are done using `bids`/`asks`:
        // erasing a map node destroys the list those references point to.
        if (bids.empty())
            bids_.erase(bidLevel);
        if (asks.empty())
            asks_.erase(askLevel);
    }

    // An immediate-or-cancel order (FAK / Market; FOK is pre-checked so it never
    // has a remainder) that was only partially filled is now resting at the top
    // of its side; kill the remainder. Copy the id before cancelling.
    if (!bids_.empty()) {
        const OrderPointer& top = bids_.begin()->second.front();
        if (IsImmediateOrCancel(top->GetOrderType())) {
            const OrderId id = top->GetOrderId();
            (void)CancelOrder(id);
        }
    }
    if (!asks_.empty()) {
        const OrderPointer& top = asks_.begin()->second.front();
        if (IsImmediateOrCancel(top->GetOrderType())) {
            const OrderId id = top->GetOrderId();
            (void)CancelOrder(id);
        }
    }

    return trades;
}

AddResult OrderBook::AddOrder(OrderPointer order) {
    if (!order)
        return std::unexpected(RejectReason::InvalidOrder);
    if (order->GetRemainingQuantity() == 0)
        return std::unexpected(RejectReason::ZeroQuantity);
    if (orders_.contains(order->GetOrderId()))
        return std::unexpected(RejectReason::DuplicateOrderId);

    switch (order->GetOrderType()) {
        case OrderType::FillAndKill:
        case OrderType::Market:
            if (!CanMatch(order->GetSide(), order->GetPrice()))
                return std::unexpected(RejectReason::CannotMatch);
            break;
        case OrderType::FillOrKill:
            // Check BEFORE touching the book: a FOK must be all-or-nothing.
            if (!CanFullyFill(order->GetSide(), order->GetPrice(), order->GetRemainingQuantity()))
                return std::unexpected(RejectReason::CannotFullyFill);
            break;
        case OrderType::GoodTillCancel:
        case OrderType::GoodForDay:
            break;
    }

    OrderPointers::iterator location;
    if (order->GetSide() == Side::Buy) {
        auto& level = bids_[order->GetPrice()];
        level.push_back(order);
        location = std::prev(level.end());
    } else {
        auto& level = asks_[order->GetPrice()];
        level.push_back(order);
        location = std::prev(level.end());
    }

    // Read id/side BEFORE moving `order`: the evaluation order of function
    // arguments is unspecified, so `emplace(order->GetOrderId(), std::move(order))`
    // may move first and then dereference a null pointer.
    const OrderId id = order->GetOrderId();
    const Side side = order->GetSide();
    orders_.emplace(id, OrderEntry{std::move(order), location});
    return MatchOrders(side);
}

CancelResult OrderBook::CancelOrder(OrderId orderId) {
    auto it = orders_.find(orderId);
    if (it == orders_.end())
        return std::unexpected(RejectReason::UnknownOrderId);

    // Copy the entry BEFORE erasing it from the map; a reference/structured
    // binding into the map would dangle after erase().
    const OrderEntry entry = it->second;
    orders_.erase(it);

    const Price price = entry.order_->GetPrice();
    if (entry.order_->GetSide() == Side::Buy)
        EraseFromLevel(bids_, price, entry.location_);
    else
        EraseFromLevel(asks_, price, entry.location_);
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
    const OrderType type = it->second.order_->GetOrderType();  // copy before cancel
    (void)CancelOrder(order.GetOrderId());
    return AddOrder(order.ToOrderPointer(type));
}

std::vector<OrderId> OrderBook::CancelGoodForDayOrders() {
    // Collect first: cancelling while iterating orders_ would invalidate the iterator.
    std::vector<OrderId> ids;
    for (const auto& [id, entry] : orders_)
        if (entry.order_->GetOrderType() == OrderType::GoodForDay)
            ids.push_back(id);
    std::ranges::sort(ids);  // unordered_map order is unspecified; make output deterministic

    for (const OrderId id : ids)
        (void)CancelOrder(id);
    return ids;
}

OrderBookLevelInfos OrderBook::GetOrderInfos() const {
    auto makeLevelInfo = [](Price price, const OrderPointers& orders) {
        const Quantity total = std::accumulate(
            orders.begin(), orders.end(), Quantity{0},
            [](Quantity sum, const OrderPointer& o) { return sum + o->GetRemainingQuantity(); });
        return LevelInfo{price, total};
    };

    LevelInfos bidInfos;
    LevelInfos askInfos;
    bidInfos.reserve(bids_.size());
    askInfos.reserve(asks_.size());

    for (const auto& [price, orders] : bids_)
        bidInfos.push_back(makeLevelInfo(price, orders));
    for (const auto& [price, orders] : asks_)
        askInfos.push_back(makeLevelInfo(price, orders));

    return OrderBookLevelInfos{std::move(bidInfos), std::move(askInfos)};
}

}  // namespace orderbook
