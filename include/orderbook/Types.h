#pragma once

#include <cstdint>

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
};

}  // namespace orderbook
