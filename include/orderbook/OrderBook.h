#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

#include "orderbook/LevelInfo.h"
#include "orderbook/Order.h"
#include "orderbook/OrderModify.h"
#include "orderbook/Trade.h"
#include "orderbook/Types.h"

namespace orderbook {

using AddResult = std::expected<Trades, RejectReason>;
using CancelResult = std::expected<void, RejectReason>;

// Single-instrument limit order book with price-time priority.
//
//   bids_/asks_ : price level -> FIFO queue of orders    (O(log L) to find a level)
//   orders_     : id -> {order, position in its queue}   (O(1) cancel)
class OrderBook {
public:
    // Adds an order and matches it. On success returns the trades produced
    // (possibly none); on rejection the book is unchanged.
    //   InvalidOrder / ZeroQuantity / DuplicateOrderId : bad request
    //   CannotMatch     : FillAndKill or Market with nothing to trade against
    //   CannotFullyFill : FillOrKill without enough liquidity within its limit
    AddResult AddOrder(OrderPointer order);

    // UnknownOrderId if the id is not live.
    CancelResult CancelOrder(OrderId orderId);

    // Cancel/replace: the order keeps its type but loses time priority.
    // Validated before the original is touched, so a rejected modify leaves it live.
    AddResult ModifyOrder(const OrderModify& order);

    // End-of-day: cancels every GoodForDay order. Returns their ids (ascending).
    std::vector<OrderId> CancelGoodForDayOrders();

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
    bool CanFullyFill(Side side, Price price, Quantity quantity) const;
    Trades MatchOrders(Side aggressorSide);
};

}  // namespace orderbook
