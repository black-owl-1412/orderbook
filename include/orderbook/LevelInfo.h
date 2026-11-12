#pragma once

#include <vector>

#include "orderbook/Types.h"

namespace orderbook {

// Aggregated quantity resting at one price level.
struct LevelInfo {
    Price price_;
    Quantity quantity_;
};

using LevelInfos = std::vector<LevelInfo>;

// Depth snapshot: bids best (highest) first, asks best (lowest) first.
class OrderBookLevelInfos {
public:
    OrderBookLevelInfos(LevelInfos bids, LevelInfos asks)
        : bids_{std::move(bids)}, asks_{std::move(asks)} {}

    const LevelInfos& GetBids() const { return bids_; }
    const LevelInfos& GetAsks() const { return asks_; }

private:
    LevelInfos bids_;
    LevelInfos asks_;
};

}  // namespace orderbook
