#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include "orderbook/OrderBook.h"
#include "orderbook/ToString.h"

using namespace orderbook;

namespace {

OrderPointer Make(OrderType type, OrderId id, Side side, Price price, Quantity qty) {
    return std::make_shared<Order>(type, id, side, qty, price);
}
OrderPointer Gtc(OrderId id, Side side, Price price, Quantity qty) {
    return Make(OrderType::GoodTillCancel, id, side, price, qty);
}
OrderPointer Gfd(OrderId id, Side side, Price price, Quantity qty) {
    return Make(OrderType::GoodForDay, id, side, price, qty);
}
OrderPointer Fak(OrderId id, Side side, Price price, Quantity qty) {
    return Make(OrderType::FillAndKill, id, side, price, qty);
}
OrderPointer Fok(OrderId id, Side side, Price price, Quantity qty) {
    return Make(OrderType::FillOrKill, id, side, price, qty);
}
OrderPointer Mkt(OrderId id, Side side, Quantity qty) {
    return std::make_shared<Order>(id, side, qty);
}

// Unwraps an accepted result; records a test failure if it was rejected.
Trades Accept(const AddResult& result) {
    if (!result) {
        ADD_FAILURE() << "unexpected reject: " << ToString(result.error());
        return {};
    }
    return *result;
}

void ExpectReject(const AddResult& result, RejectReason reason) {
    ASSERT_FALSE(result.has_value()) << "expected reject " << ToString(reason);
    EXPECT_EQ(result.error(), reason) << "got " << ToString(result.error());
}

void ExpectTrade(const Trade& t, OrderId bidId, OrderId askId, Price price, Quantity qty) {
    EXPECT_EQ(t.GetBidTrade().orderId_, bidId);
    EXPECT_EQ(t.GetAskTrade().orderId_, askId);
    EXPECT_EQ(t.GetBidTrade().price_, price);
    EXPECT_EQ(t.GetAskTrade().price_, price);
    EXPECT_EQ(t.GetBidTrade().quantity_, qty);
    EXPECT_EQ(t.GetAskTrade().quantity_, qty);
}

void ExpectLevel(const LevelInfo& level, Price price, Quantity qty) {
    EXPECT_EQ(level.price_, price);
    EXPECT_EQ(level.quantity_, qty);
}

}  // namespace

// ---------- Order / OrderModify ----------

TEST(Order, FillReducesRemainingQuantity) {
    Order o{OrderType::GoodTillCancel, 1, Side::Buy, 10, 100};
    o.Fill(4);
    EXPECT_EQ(o.GetRemainingQuantity(), 6u);
    EXPECT_EQ(o.GetFilledQuantity(), 4u);
    EXPECT_FALSE(o.IsFilled());
    o.Fill(6);
    EXPECT_TRUE(o.IsFilled());
}

TEST(Order, OverfillThrows) {
    Order o{OrderType::GoodTillCancel, 1, Side::Buy, 5, 100};
    EXPECT_THROW(o.Fill(6), std::logic_error);
    EXPECT_EQ(o.GetRemainingQuantity(), 5u);
}

TEST(Order, MarketOrderUsesMostAggressivePrice) {
    EXPECT_EQ(Order(1, Side::Buy, 5).GetPrice(), std::numeric_limits<Price>::max());
    EXPECT_EQ(Order(2, Side::Sell, 5).GetPrice(), std::numeric_limits<Price>::min());
    // Price passed to the general constructor is ignored for Market.
    EXPECT_EQ(Order(OrderType::Market, 3, Side::Buy, 5, 100).GetPrice(),
              std::numeric_limits<Price>::max());
    EXPECT_EQ(Order(1, Side::Buy, 5).GetOrderType(), OrderType::Market);
}

TEST(OrderModify, KeepsQuantity) {  // regression: `quantity_{quantity_}` self-init
    OrderModify m{7, Side::Sell, 42, 105};
    EXPECT_EQ(m.GetQuantity(), 42u);
    auto o = m.ToOrderPointer(OrderType::FillAndKill);
    EXPECT_EQ(o->GetOrderId(), 7u);
    EXPECT_EQ(o->GetSide(), Side::Sell);
    EXPECT_EQ(o->GetPrice(), 105);
    EXPECT_EQ(o->GetInitialQuantity(), 42u);
    EXPECT_EQ(o->GetOrderType(), OrderType::FillAndKill);
}

// ---------- Adding / resting / rejects ----------

TEST(OrderBook, NonCrossingOrdersRest) {
    OrderBook book;
    EXPECT_TRUE(Accept(book.AddOrder(Gtc(1, Side::Buy, 100, 10))).empty());
    EXPECT_TRUE(Accept(book.AddOrder(Gtc(2, Side::Sell, 101, 5))).empty());
    EXPECT_EQ(book.Size(), 2u);

    const auto infos = book.GetOrderInfos();
    ASSERT_EQ(infos.GetBids().size(), 1u);
    ASSERT_EQ(infos.GetAsks().size(), 1u);
    ExpectLevel(infos.GetBids()[0], 100, 10);
    ExpectLevel(infos.GetAsks()[0], 101, 5);
}

TEST(OrderBook, DuplicateIdRejected) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 100, 10)));
    ExpectReject(book.AddOrder(Gtc(1, Side::Buy, 99, 3)), RejectReason::DuplicateOrderId);
    EXPECT_EQ(book.Size(), 1u);
    const auto infos = book.GetOrderInfos();
    ASSERT_EQ(infos.GetBids().size(), 1u);
    ExpectLevel(infos.GetBids()[0], 100, 10);
}

TEST(OrderBook, ZeroQuantityRejected) {
    OrderBook book;
    ExpectReject(book.AddOrder(Gtc(1, Side::Buy, 100, 0)), RejectReason::ZeroQuantity);
    EXPECT_EQ(book.Size(), 0u);
    EXPECT_TRUE(book.GetOrderInfos().GetBids().empty());
}

TEST(OrderBook, NullOrderRejected) {
    OrderBook book;
    ExpectReject(book.AddOrder(nullptr), RejectReason::InvalidOrder);
    EXPECT_EQ(book.Size(), 0u);
}

TEST(OrderBook, LevelsAggregatedAndSorted) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 100, 1)));
    Accept(book.AddOrder(Gtc(2, Side::Buy, 102, 2)));
    Accept(book.AddOrder(Gtc(3, Side::Buy, 101, 3)));
    Accept(book.AddOrder(Gtc(4, Side::Buy, 102, 4)));
    Accept(book.AddOrder(Gtc(5, Side::Sell, 105, 5)));
    Accept(book.AddOrder(Gtc(6, Side::Sell, 103, 6)));
    Accept(book.AddOrder(Gtc(7, Side::Sell, 105, 7)));

    const auto infos = book.GetOrderInfos();
    const auto& bids = infos.GetBids();
    const auto& asks = infos.GetAsks();
    ASSERT_EQ(bids.size(), 3u);
    ASSERT_EQ(asks.size(), 2u);
    ExpectLevel(bids[0], 102, 6);  // best bid first (descending)
    ExpectLevel(bids[1], 101, 3);
    ExpectLevel(bids[2], 100, 1);
    ExpectLevel(asks[0], 103, 6);  // best ask first (ascending)
    ExpectLevel(asks[1], 105, 12);
}

// ---------- Matching ----------

TEST(OrderBook, AggressiveBuyTradesAtRestingAskPrice) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Sell, 100, 10)));
    const auto trades = Accept(book.AddOrder(Gtc(2, Side::Buy, 105, 10)));
    ASSERT_EQ(trades.size(), 1u);
    ExpectTrade(trades[0], 2, 1, 100, 10);
    EXPECT_EQ(book.Size(), 0u);
    EXPECT_TRUE(book.GetOrderInfos().GetBids().empty());
    EXPECT_TRUE(book.GetOrderInfos().GetAsks().empty());
}

TEST(OrderBook, AggressiveSellTradesAtRestingBidPrice) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 100, 10)));
    const auto trades = Accept(book.AddOrder(Gtc(2, Side::Sell, 95, 10)));
    ASSERT_EQ(trades.size(), 1u);
    ExpectTrade(trades[0], 1, 2, 100, 10);
    EXPECT_EQ(book.Size(), 0u);
}

TEST(OrderBook, PartialFillRestingRemainderStays) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Sell, 100, 10)));
    const auto trades = Accept(book.AddOrder(Gtc(2, Side::Buy, 100, 4)));
    ASSERT_EQ(trades.size(), 1u);
    ExpectTrade(trades[0], 2, 1, 100, 4);
    EXPECT_EQ(book.Size(), 1u);
    const auto infos = book.GetOrderInfos();
    EXPECT_TRUE(infos.GetBids().empty());
    ASSERT_EQ(infos.GetAsks().size(), 1u);
    ExpectLevel(infos.GetAsks()[0], 100, 6);
}

TEST(OrderBook, PartialFillAggressorRemainderRests) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Sell, 100, 4)));
    const auto trades = Accept(book.AddOrder(Gtc(2, Side::Buy, 100, 10)));
    ASSERT_EQ(trades.size(), 1u);
    ExpectTrade(trades[0], 2, 1, 100, 4);
    EXPECT_EQ(book.Size(), 1u);
    const auto infos = book.GetOrderInfos();
    EXPECT_TRUE(infos.GetAsks().empty());
    ASSERT_EQ(infos.GetBids().size(), 1u);
    ExpectLevel(infos.GetBids()[0], 100, 6);
}

TEST(OrderBook, SweepsLevelsInPriceThenTimePriority) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Sell, 101, 5)));
    Accept(book.AddOrder(Gtc(2, Side::Sell, 100, 5)));
    Accept(book.AddOrder(Gtc(3, Side::Sell, 100, 5)));
    Accept(book.AddOrder(Gtc(4, Side::Sell, 102, 5)));  // out of the buyer's range

    const auto trades = Accept(book.AddOrder(Gtc(5, Side::Buy, 101, 12)));
    ASSERT_EQ(trades.size(), 3u);
    ExpectTrade(trades[0], 5, 2, 100, 5);  // best price first, then FIFO within level
    ExpectTrade(trades[1], 5, 3, 100, 5);
    ExpectTrade(trades[2], 5, 1, 101, 2);

    EXPECT_EQ(book.Size(), 2u);  // #1 (3 left) and #4
    const auto infos = book.GetOrderInfos();
    EXPECT_TRUE(infos.GetBids().empty());
    ASSERT_EQ(infos.GetAsks().size(), 2u);
    ExpectLevel(infos.GetAsks()[0], 101, 3);
    ExpectLevel(infos.GetAsks()[1], 102, 5);
}

TEST(OrderBook, ManySmallRestingOrdersFilledByOneAggressor) {
    OrderBook book;
    for (OrderId id = 1; id <= 100; ++id)
        Accept(book.AddOrder(Gtc(id, Side::Buy, 100, 1)));
    const auto trades = Accept(book.AddOrder(Gtc(1000, Side::Sell, 100, 100)));
    ASSERT_EQ(trades.size(), 100u);
    for (OrderId id = 1; id <= 100; ++id)
        ExpectTrade(trades[id - 1], id, 1000, 100, 1);
    EXPECT_EQ(book.Size(), 0u);
}

// ---------- FillAndKill ----------

TEST(FillAndKill, OnEmptyBookRejected) {
    OrderBook book;
    ExpectReject(book.AddOrder(Fak(1, Side::Buy, 100, 10)), RejectReason::CannotMatch);
    EXPECT_EQ(book.Size(), 0u);
}

TEST(FillAndKill, WithoutCrossRejected) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 100, 10)));
    ExpectReject(book.AddOrder(Fak(2, Side::Sell, 101, 10)), RejectReason::CannotMatch);
    EXPECT_EQ(book.Size(), 1u);
    EXPECT_TRUE(book.GetOrderInfos().GetAsks().empty());
}

TEST(FillAndKill, RemainderCancelled) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Sell, 100, 5)));
    const auto trades = Accept(book.AddOrder(Fak(2, Side::Buy, 100, 8)));
    ASSERT_EQ(trades.size(), 1u);
    ExpectTrade(trades[0], 2, 1, 100, 5);
    EXPECT_EQ(book.Size(), 0u);
    EXPECT_TRUE(book.GetOrderInfos().GetBids().empty());
}

TEST(FillAndKill, SweepsThenKillsRemainder) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 100, 5)));
    Accept(book.AddOrder(Gtc(2, Side::Buy, 99, 5)));
    Accept(book.AddOrder(Gtc(3, Side::Buy, 98, 5)));
    const auto trades = Accept(book.AddOrder(Fak(4, Side::Sell, 99, 20)));
    ASSERT_EQ(trades.size(), 2u);
    ExpectTrade(trades[0], 1, 4, 100, 5);
    ExpectTrade(trades[1], 2, 4, 99, 5);
    EXPECT_EQ(book.Size(), 1u);  // only #3 remains
    const auto infos = book.GetOrderInfos();
    EXPECT_TRUE(infos.GetAsks().empty());
    ASSERT_EQ(infos.GetBids().size(), 1u);
    ExpectLevel(infos.GetBids()[0], 98, 5);
}

TEST(FillAndKill, FullyFilledDoesNotRest) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Sell, 100, 10)));
    const auto trades = Accept(book.AddOrder(Fak(2, Side::Buy, 100, 4)));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(book.Size(), 1u);
    ExpectLevel(book.GetOrderInfos().GetAsks()[0], 100, 6);
}

// ---------- FillOrKill ----------

TEST(FillOrKill, FillsFullyAcrossLevels) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Sell, 100, 5)));
    Accept(book.AddOrder(Gtc(2, Side::Sell, 101, 5)));
    const auto trades = Accept(book.AddOrder(Fok(3, Side::Buy, 101, 8)));
    ASSERT_EQ(trades.size(), 2u);
    ExpectTrade(trades[0], 3, 1, 100, 5);
    ExpectTrade(trades[1], 3, 2, 101, 3);
    EXPECT_EQ(book.Size(), 1u);
    ExpectLevel(book.GetOrderInfos().GetAsks()[0], 101, 2);
}

TEST(FillOrKill, ExactQuantityAccepted) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 100, 3)));
    Accept(book.AddOrder(Gtc(2, Side::Buy, 100, 4)));
    const auto trades = Accept(book.AddOrder(Fok(3, Side::Sell, 100, 7)));
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(book.Size(), 0u);
}

TEST(FillOrKill, InsufficientQuantityRejectedBookUnchanged) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Sell, 100, 5)));
    ExpectReject(book.AddOrder(Fok(2, Side::Buy, 100, 6)), RejectReason::CannotFullyFill);
    EXPECT_EQ(book.Size(), 1u);
    const auto infos = book.GetOrderInfos();
    EXPECT_TRUE(infos.GetBids().empty());
    ExpectLevel(infos.GetAsks()[0], 100, 5);  // nothing was partially filled
}

TEST(FillOrKill, LiquidityBeyondLimitNotCounted) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Sell, 100, 5)));
    Accept(book.AddOrder(Gtc(2, Side::Sell, 102, 5)));
    ExpectReject(book.AddOrder(Fok(3, Side::Buy, 101, 8)), RejectReason::CannotFullyFill);
    EXPECT_EQ(book.Size(), 2u);
}

TEST(FillOrKill, NoCrossRejected) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 99, 5)));
    ExpectReject(book.AddOrder(Fok(2, Side::Sell, 100, 1)), RejectReason::CannotFullyFill);
    ExpectReject(book.AddOrder(Fok(3, Side::Buy, 100, 1)), RejectReason::CannotFullyFill);
    EXPECT_EQ(book.Size(), 1u);
}

// ---------- Market ----------

TEST(Market, BuySweepsAtMakerPrices) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Sell, 100, 5)));
    Accept(book.AddOrder(Gtc(2, Side::Sell, 101, 5)));
    const auto trades = Accept(book.AddOrder(Mkt(3, Side::Buy, 7)));
    ASSERT_EQ(trades.size(), 2u);
    ExpectTrade(trades[0], 3, 1, 100, 5);
    ExpectTrade(trades[1], 3, 2, 101, 2);
    EXPECT_EQ(book.Size(), 1u);
    EXPECT_TRUE(book.GetOrderInfos().GetBids().empty());
    ExpectLevel(book.GetOrderInfos().GetAsks()[0], 101, 3);
}

TEST(Market, SellSweepsAtMakerPrices) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 100, 5)));
    Accept(book.AddOrder(Gtc(2, Side::Buy, 90, 5)));
    const auto trades = Accept(book.AddOrder(Mkt(3, Side::Sell, 10)));
    ASSERT_EQ(trades.size(), 2u);
    ExpectTrade(trades[0], 1, 3, 100, 5);
    ExpectTrade(trades[1], 2, 3, 90, 5);
    EXPECT_EQ(book.Size(), 0u);
}

TEST(Market, RemainderCancelled) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Sell, 100, 5)));
    const auto trades = Accept(book.AddOrder(Mkt(2, Side::Buy, 10)));
    ASSERT_EQ(trades.size(), 1u);
    ExpectTrade(trades[0], 2, 1, 100, 5);
    EXPECT_EQ(book.Size(), 0u);
    EXPECT_TRUE(book.GetOrderInfos().GetBids().empty());
}

TEST(Market, EmptyOppositeSideRejected) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 100, 5)));  // same side only
    ExpectReject(book.AddOrder(Mkt(2, Side::Buy, 1)), RejectReason::CannotMatch);
    EXPECT_EQ(book.Size(), 1u);
    ExpectLevel(book.GetOrderInfos().GetBids()[0], 100, 5);
}

// ---------- GoodForDay ----------

TEST(GoodForDay, RestsAndMatchesLikeGtc) {
    OrderBook book;
    Accept(book.AddOrder(Gfd(1, Side::Sell, 100, 5)));
    EXPECT_EQ(book.Size(), 1u);
    const auto trades = Accept(book.AddOrder(Gfd(2, Side::Buy, 100, 3)));
    ASSERT_EQ(trades.size(), 1u);
    ExpectTrade(trades[0], 2, 1, 100, 3);
    EXPECT_EQ(book.Size(), 1u);
}

TEST(GoodForDay, EndOfDayCancelsOnlyGfdOrders) {
    OrderBook book;
    Accept(book.AddOrder(Gfd(7, Side::Buy, 98, 5)));
    Accept(book.AddOrder(Gtc(2, Side::Buy, 99, 5)));
    Accept(book.AddOrder(Gfd(3, Side::Sell, 101, 5)));
    Accept(book.AddOrder(Gtc(4, Side::Sell, 101, 5)));

    EXPECT_EQ(book.CancelGoodForDayOrders(), (std::vector<OrderId>{3, 7}));
    EXPECT_EQ(book.Size(), 2u);
    const auto infos = book.GetOrderInfos();
    ASSERT_EQ(infos.GetBids().size(), 1u);
    ASSERT_EQ(infos.GetAsks().size(), 1u);
    ExpectLevel(infos.GetBids()[0], 99, 5);
    ExpectLevel(infos.GetAsks()[0], 101, 5);

    EXPECT_TRUE(book.CancelGoodForDayOrders().empty());  // idempotent
}

TEST(GoodForDay, ModifiedOrderStaysGfd) {
    OrderBook book;
    Accept(book.AddOrder(Gfd(1, Side::Buy, 99, 5)));
    Accept(book.ModifyOrder(OrderModify{1, Side::Buy, 8, 100}));
    EXPECT_EQ(book.CancelGoodForDayOrders(), (std::vector<OrderId>{1}));
    EXPECT_EQ(book.Size(), 0u);
}

// ---------- Cancel ----------

TEST(Cancel, RemovesOrderAndEmptyLevel) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 100, 10)));
    Accept(book.AddOrder(Gtc(2, Side::Sell, 101, 10)));
    EXPECT_TRUE(book.CancelOrder(1).has_value());
    EXPECT_EQ(book.Size(), 1u);
    EXPECT_TRUE(book.GetOrderInfos().GetBids().empty());
    EXPECT_TRUE(book.CancelOrder(2).has_value());
    EXPECT_EQ(book.Size(), 0u);
    EXPECT_TRUE(book.GetOrderInfos().GetAsks().empty());
}

TEST(Cancel, UnknownOrTwiceRejected) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 100, 10)));
    EXPECT_EQ(book.CancelOrder(42).error(), RejectReason::UnknownOrderId);
    EXPECT_EQ(book.Size(), 1u);
    EXPECT_TRUE(book.CancelOrder(1).has_value());
    EXPECT_EQ(book.CancelOrder(1).error(), RejectReason::UnknownOrderId);
    EXPECT_EQ(book.Size(), 0u);
}

TEST(Cancel, MiddleOfLevelKeepsFifoOfOthers) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Sell, 100, 3)));
    Accept(book.AddOrder(Gtc(2, Side::Sell, 100, 3)));
    Accept(book.AddOrder(Gtc(3, Side::Sell, 100, 3)));
    EXPECT_TRUE(book.CancelOrder(2).has_value());
    ExpectLevel(book.GetOrderInfos().GetAsks()[0], 100, 6);

    const auto trades = Accept(book.AddOrder(Gtc(4, Side::Buy, 100, 6)));
    ASSERT_EQ(trades.size(), 2u);
    ExpectTrade(trades[0], 4, 1, 100, 3);
    ExpectTrade(trades[1], 4, 3, 100, 3);
    EXPECT_EQ(book.Size(), 0u);
}

TEST(Cancel, FilledOrderIsUnknown) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Sell, 100, 5)));
    Accept(book.AddOrder(Gtc(2, Side::Buy, 100, 5)));
    EXPECT_EQ(book.CancelOrder(1).error(), RejectReason::UnknownOrderId);
    EXPECT_EQ(book.CancelOrder(2).error(), RejectReason::UnknownOrderId);
    EXPECT_EQ(book.Size(), 0u);
}

// ---------- Modify ----------

TEST(Modify, UnknownRejected) {
    OrderBook book;
    ExpectReject(book.ModifyOrder(OrderModify{1, Side::Buy, 10, 100}),
                 RejectReason::UnknownOrderId);
    EXPECT_EQ(book.Size(), 0u);
}

TEST(Modify, ZeroQuantityRejectedOriginalKept) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 100, 10)));
    ExpectReject(book.ModifyOrder(OrderModify{1, Side::Buy, 0, 100}), RejectReason::ZeroQuantity);
    EXPECT_EQ(book.Size(), 1u);
    ExpectLevel(book.GetOrderInfos().GetBids()[0], 100, 10);
}

TEST(Modify, ChangesPriceAndQuantity) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 100, 10)));
    EXPECT_TRUE(Accept(book.ModifyOrder(OrderModify{1, Side::Buy, 25, 98})).empty());
    EXPECT_EQ(book.Size(), 1u);
    const auto infos = book.GetOrderInfos();
    ASSERT_EQ(infos.GetBids().size(), 1u);
    ExpectLevel(infos.GetBids()[0], 98, 25);
}

TEST(Modify, LosesTimePriority) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 100, 5)));
    Accept(book.AddOrder(Gtc(2, Side::Buy, 100, 5)));
    Accept(book.ModifyOrder(OrderModify{1, Side::Buy, 5, 100}));  // #1 goes to the back

    const auto trades = Accept(book.AddOrder(Gtc(3, Side::Sell, 100, 5)));
    ASSERT_EQ(trades.size(), 1u);
    ExpectTrade(trades[0], 2, 3, 100, 5);
}

TEST(Modify, CanTriggerMatch) {
    OrderBook book;
    Accept(book.AddOrder(Gtc(1, Side::Buy, 99, 5)));
    Accept(book.AddOrder(Gtc(2, Side::Sell, 101, 5)));
    const auto trades = Accept(book.ModifyOrder(OrderModify{1, Side::Buy, 5, 102}));
    ASSERT_EQ(trades.size(), 1u);
    ExpectTrade(trades[0], 1, 2, 101, 5);  // maker (#2) price
    EXPECT_EQ(book.Size(), 0u);
}
