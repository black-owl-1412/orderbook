#pragma once

#include <cstdint>
#include <limits>

namespace orderbook {

// Prices are integer ticks (never floating point: 0.1 + 0.2 != 0.3).
using Price = std::int32_t;
using Quantity = std::uint32_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t {
    Buy,
    Sell,
};

enum class OrderType : std::uint8_t {
    GoodTillCancel,  // rests in the book until filled or cancelled
    FillAndKill,     // (a.k.a. IOC) match what you can immediately, cancel the rest
    FillOrKill,      // fill the whole quantity immediately, or reject entirely
    GoodForDay,      // like GoodTillCancel, but cancelled at end of trading day
    Market,          // no limit price: take liquidity at any price, cancel the rest
};

// Why the book refused a request. Returned via std::expected.
enum class RejectReason : std::uint8_t {
    InvalidOrder,     // null order pointer
    ZeroQuantity,     // quantity must be > 0
    DuplicateOrderId, // an order with this id is already live
    UnknownOrderId,   // cancel/modify of an id that is not live
    CannotMatch,      // FillAndKill / Market with nothing to trade against
    CannotFullyFill,  // FillOrKill without enough liquidity at acceptable prices
};

// Orders that must never rest in the book.
constexpr bool IsImmediateOrCancel(OrderType type) {
    return type == OrderType::FillAndKill || type == OrderType::FillOrKill ||
           type == OrderType::Market;
}

// A market order is stored with the most aggressive possible price, so the normal
// limit-price matching logic lets it cross any resting order on the other side.
constexpr Price MarketPrice(Side side) {
    return side == Side::Buy ? std::numeric_limits<Price>::max()
                             : std::numeric_limits<Price>::min();
}

}  // namespace orderbook
