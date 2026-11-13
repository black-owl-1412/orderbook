#pragma once

#include <cstddef>
#include <expected>
#if defined(ORDERBOOK_FLAT_LEVELS)
#include <flat_map>
#endif
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "orderbook/LevelInfo.h"
#include "orderbook/ObjectPool.h"
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
//   orders_     : id -> order node                       (O(1) cancel)
//
// Each side is sorted so that its BEST level is the LAST element. Trading
// happens at the best level, so levels are created and removed mostly at the
// end of the container. That is O(1) for a sorted array (std::flat_map, enabled
// with ORDERBOOK_FLAT_LEVELS) and costs nothing extra for std::map.
//
// Resting orders live in pooled nodes that are linked directly to their
// neighbours in the queue (an intrusive list), so adding or removing an order
// allocates nothing and unlinking needs no search.
class OrderBook {
public:
    // Adds an order and matches it. On success returns the trades produced
    // (possibly none); on rejection the book is unchanged.
    //   ZeroQuantity / DuplicateOrderId : bad request
    //   CannotMatch     : FillAndKill or Market with nothing to trade against
    //   CannotFullyFill : FillOrKill without enough liquidity within its limit
    // The book keeps its own copy of the order.
    AddResult AddOrder(const Order& order);

    // Convenience overload; InvalidOrder if the pointer is null.
    AddResult AddOrder(const OrderPointer& order);

    // UnknownOrderId if the id is not live.
    CancelResult CancelOrder(OrderId orderId);

    // Cancel/replace: the order keeps its type but loses time priority.
    // Validated before the original is touched, so a rejected modify leaves it live.
    AddResult ModifyOrder(const OrderModify& order);

    // End-of-day: cancels every GoodForDay order. Returns their ids (ascending).
    std::vector<OrderId> CancelGoodForDayOrders();

    std::size_t Size() const { return orders_.size(); }

    std::optional<Price> BestBid() const {
        if (bids_.empty())
            return std::nullopt;
        return (*bids_.rbegin()).first;
    }
    std::optional<Price> BestAsk() const {
        if (asks_.empty())
            return std::nullopt;
        return (*asks_.rbegin()).first;
    }

    OrderBookLevelInfos GetOrderInfos() const;

private:
    struct OrderNode {
        explicit OrderNode(const Order& o) : order{o} {}
        Order order;
        OrderNode* prev = nullptr;
        OrderNode* next = nullptr;
    };

    // A price level: FIFO queue (head trades first) plus its running total, kept
    // in sync on every add / fill / cancel so depth snapshots and FOK checks are
    // O(levels), not O(orders).
    struct Level {
        OrderNode* head = nullptr;
        OrderNode* tail = nullptr;
        Quantity quantity = 0;

        bool Empty() const { return head == nullptr; }

        void PushBack(OrderNode* node) {
            node->prev = tail;
            node->next = nullptr;
            (tail ? tail->next : head) = node;
            tail = node;
        }

        void Unlink(OrderNode* node) {
            (node->prev ? node->prev->next : head) = node->next;
            (node->next ? node->next->prev : tail) = node->prev;
        }
    };

#if defined(ORDERBOOK_FLAT_LEVELS)
    template <typename Compare>
    using LevelMap = std::flat_map<Price, Level, Compare>;
#else
    template <typename Compare>
    using LevelMap = std::map<Price, Level, Compare>;
#endif

    LevelMap<std::less<Price>> bids_;     // ascending: best (highest) bid is last
    LevelMap<std::greater<Price>> asks_;  // descending: best (lowest) ask is last
    std::unordered_map<OrderId, OrderNode*> orders_;
    ObjectPool<OrderNode> pool_;

    bool CanMatch(Side side, Price price) const;
    bool CanFullyFill(Side side, Price price, Quantity quantity) const;
    template <typename Levels>
    void Match(Levels& opposite, Order& taker, Trades& trades);
};

}  // namespace orderbook
