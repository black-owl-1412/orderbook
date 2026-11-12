#pragma once

#include <memory>

#include "orderbook/Order.h"
#include "orderbook/Types.h"

namespace orderbook {

// A request to replace an existing order's side/price/quantity.
class OrderModify {
public:
    OrderModify(OrderId orderId, Side side, Quantity quantity, Price price)
        : orderId_{orderId},
          side_{side},
          price_{price},
          quantity_{quantity} {}  // was `quantity_{quantity_}` (self-init -> garbage)

    OrderId GetOrderId() const { return orderId_; }
    Side GetSide() const { return side_; }
    Price GetPrice() const { return price_; }
    Quantity GetQuantity() const { return quantity_; }

    OrderPointer ToOrderPointer(OrderType type) const {
        return std::make_shared<Order>(type, orderId_, side_, quantity_, price_);
    }

private:
    OrderId orderId_;
    Side side_;
    Price price_;
    Quantity quantity_;
};

}  // namespace orderbook
