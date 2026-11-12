#pragma once

#include <vector>

#include "orderbook/Types.h"

namespace orderbook {

struct TradeInfo {
    OrderId orderId_;
    Price price_;  // execution price (the resting/maker order's price)
    Quantity quantity_;
};

// One execution between a bid and an ask. Both legs carry the same price and
// quantity; they are kept separate so each side can be reported to its owner.
class Trade {
public:
    Trade(const TradeInfo& bidTrade, const TradeInfo& askTrade)
        : bidTrade_{bidTrade}, askTrade_{askTrade} {}

    const TradeInfo& GetBidTrade() const { return bidTrade_; }
    const TradeInfo& GetAskTrade() const { return askTrade_; }
    Price GetPrice() const { return bidTrade_.price_; }
    Quantity GetQuantity() const { return bidTrade_.quantity_; }

private:
    TradeInfo bidTrade_;
    TradeInfo askTrade_;
};

using Trades = std::vector<Trade>;

}  // namespace orderbook
