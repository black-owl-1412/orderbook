#pragma once

#include <string_view>
#include <utility>

#include "orderbook/Types.h"

namespace orderbook {

// std::unreachable() after an exhaustive switch: tells the compiler (and reader)
// every enumerator is handled. With -Wall, adding a new enumerator without a
// case triggers -Wswitch.

constexpr std::string_view ToString(Side side) {
    switch (side) {
        case Side::Buy: return "BUY";
        case Side::Sell: return "SELL";
    }
    std::unreachable();
}

constexpr std::string_view ToString(OrderType type) {
    switch (type) {
        case OrderType::GoodTillCancel: return "GTC";
        case OrderType::FillAndKill: return "FAK";
        case OrderType::FillOrKill: return "FOK";
        case OrderType::GoodForDay: return "GFD";
        case OrderType::Market: return "MKT";
    }
    std::unreachable();
}

constexpr std::string_view ToString(RejectReason reason) {
    switch (reason) {
        case RejectReason::InvalidOrder: return "InvalidOrder";
        case RejectReason::ZeroQuantity: return "ZeroQuantity";
        case RejectReason::DuplicateOrderId: return "DuplicateOrderId";
        case RejectReason::UnknownOrderId: return "UnknownOrderId";
        case RejectReason::CannotMatch: return "CannotMatch";
        case RejectReason::CannotFullyFill: return "CannotFullyFill";
    }
    std::unreachable();
}

}  // namespace orderbook
