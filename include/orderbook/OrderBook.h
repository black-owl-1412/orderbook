#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <unordered_map>

#include "orderbook/LevelInfo.h"
#include "orderbook/Order.h"
#include "orderbook/OrderModify.h"
#include "orderbook/Trade.h"
#include "orderbook/Types.h"

namespace orderbook {

// Single-instrument limit order book with price-time priority.
//
//   bids_/asks_ : price level -> FIFO queue of orders    (O(log L) to find a level)
//   orders_     : id -> {order, position in its queue}   (O(1) cancel)
class OrderBook {
public:
    // Adds an order and matches it. Returns the trades produced (possibly none).
    // Rejected (returns no trades, book unchanged): duplicate id, zero quantity,
    // FillAndKill that cannot match immediately.
    Trades AddOrder(OrderPointer order);

    // No-op if the id is unknown.
    void CancelOrder(OrderId orderId);

    // Cancel/replace: the order keeps its type but loses time priority.
    Trades ModifyOrder(const OrderModify& order);

    std::size_t Size() const { return orders_.size(); }

    OrderBookLevelInfos GetOrderInfos() const;

private:
    struct OrderEntry {
        OrderPointer order_{nullptr};
        OrderPointers::iterator location_;
    };

    std::map<Price, OrderPointers, std::greater<Price>> bids_;  // best (highest) first
    std::map<Price, OrderPointers, std::less<Price>> asks_;     // best (lowest) first
    std::unordered_map<OrderId, OrderEntry> orders_;

    bool CanMatch(Side side, Price price) const;
    Trades MatchOrders(Side aggressorSide);
};

}  // namespace orderbook
