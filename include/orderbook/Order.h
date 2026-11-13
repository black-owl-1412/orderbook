#pragma once

#include <memory>
#include <stdexcept>

#include "orderbook/Types.h"

namespace orderbook {

class Order {
public:
    // For OrderType::Market the given price is ignored and replaced by MarketPrice(side).
    Order(OrderType orderType, OrderId orderId, Side side, Quantity quantity, Price price)
        : orderType_{orderType},
          orderId_{orderId},
          side_{side},
          price_{orderType == OrderType::Market ? MarketPrice(side) : price},
          initialQuantity_{quantity},
          remainingQuantity_{quantity} {}

    // Market order convenience constructor (no price).
    Order(OrderId orderId, Side side, Quantity quantity)
        : Order{OrderType::Market, orderId, side, quantity, MarketPrice(side)} {}

    OrderType GetOrderType() const { return orderType_; }
    OrderId GetOrderId() const { return orderId_; }
    Side GetSide() const { return side_; }
    Price GetPrice() const { return price_; }
    Quantity GetInitialQuantity() const { return initialQuantity_; }
    Quantity GetRemainingQuantity() const { return remainingQuantity_; }
    Quantity GetFilledQuantity() const { return initialQuantity_ - remainingQuantity_; }
    bool IsFilled() const { return remainingQuantity_ == 0; }

    void Fill(Quantity quantity) {
        // Quantity is unsigned: without this guard an overfill would silently
        // wrap around to ~4 billion instead of failing.
        if (quantity > remainingQuantity_)
            throw std::logic_error("Order cannot be filled for more than its remaining quantity");
        remainingQuantity_ -= quantity;
    }

private:
    OrderType orderType_;
    OrderId orderId_;
    Side side_;
    Price price_;
    Quantity initialQuantity_;
    Quantity remainingQuantity_;
};

// Convenience for callers. The book copies the order into its own pooled
// storage, so it never shares ownership with the caller.
using OrderPointer = std::shared_ptr<Order>;

}  // namespace orderbook
