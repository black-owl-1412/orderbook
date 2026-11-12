#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

#include "orderbook/OrderBook.h"

using namespace orderbook;

namespace {

OrderPointer Gtc(OrderId id, Side side, Price price, Quantity qty) {
    return std::make_shared<Order>(OrderType::GoodTillCancel, id, side, qty, price);
}

OrderPointer Fak(OrderId id, Side side, Price price, Quantity qty) {
    return std::make_shared<Order>(OrderType::FillAndKill, id, side, qty, price);
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

// ---------- Adding / resting ----------

TEST(OrderBook, NonCrossingOrdersRest) {
    OrderBook book;
    EXPECT_TRUE(book.AddOrder(Gtc(1, Side::Buy, 100, 10)).empty());
    EXPECT_TRUE(book.AddOrder(Gtc(2, Side::Sell, 101, 5)).empty());
    EXPECT_EQ(book.Size(), 2u);

    const auto infos = book.GetOrderInfos();
    ASSERT_EQ(infos.GetBids().size(), 1u);
    ASSERT_EQ(infos.GetAsks().size(), 1u);
    ExpectLevel(infos.GetBids()[0], 100, 10);
    ExpectLevel(infos.GetAsks()[0], 101, 5);
}

TEST(OrderBook, DuplicateIdRejected) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Buy, 100, 10));
    EXPECT_TRUE(book.AddOrder(Gtc(1, Side::Buy, 99, 3)).empty());
    EXPECT_EQ(book.Size(), 1u);
    const auto infos = book.GetOrderInfos();
    ASSERT_EQ(infos.GetBids().size(), 1u);
    ExpectLevel(infos.GetBids()[0], 100, 10);
}

TEST(OrderBook, ZeroQuantityRejected) {
    OrderBook book;
    EXPECT_TRUE(book.AddOrder(Gtc(1, Side::Buy, 100, 0)).empty());
    EXPECT_EQ(book.Size(), 0u);
    EXPECT_TRUE(book.GetOrderInfos().GetBids().empty());
}

TEST(OrderBook, NullOrderRejected) {
    OrderBook book;
    EXPECT_TRUE(book.AddOrder(nullptr).empty());
    EXPECT_EQ(book.Size(), 0u);
}

TEST(OrderBook, LevelsAggregatedAndSorted) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Buy, 100, 1));
    book.AddOrder(Gtc(2, Side::Buy, 102, 2));
    book.AddOrder(Gtc(3, Side::Buy, 101, 3));
    book.AddOrder(Gtc(4, Side::Buy, 102, 4));
    book.AddOrder(Gtc(5, Side::Sell, 105, 5));
    book.AddOrder(Gtc(6, Side::Sell, 103, 6));
    book.AddOrder(Gtc(7, Side::Sell, 105, 7));

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
    book.AddOrder(Gtc(1, Side::Sell, 100, 10));
    const auto trades = book.AddOrder(Gtc(2, Side::Buy, 105, 10));
    ASSERT_EQ(trades.size(), 1u);
    ExpectTrade(trades[0], 2, 1, 100, 10);
    EXPECT_EQ(book.Size(), 0u);
    EXPECT_TRUE(book.GetOrderInfos().GetBids().empty());
    EXPECT_TRUE(book.GetOrderInfos().GetAsks().empty());
}

TEST(OrderBook, AggressiveSellTradesAtRestingBidPrice) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Buy, 100, 10));
    const auto trades = book.AddOrder(Gtc(2, Side::Sell, 95, 10));
    ASSERT_EQ(trades.size(), 1u);
    ExpectTrade(trades[0], 1, 2, 100, 10);
    EXPECT_EQ(book.Size(), 0u);
}

TEST(OrderBook, PartialFillRestingRemainderStays) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Sell, 100, 10));
    const auto trades = book.AddOrder(Gtc(2, Side::Buy, 100, 4));
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
    book.AddOrder(Gtc(1, Side::Sell, 100, 4));
    const auto trades = book.AddOrder(Gtc(2, Side::Buy, 100, 10));
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
    book.AddOrder(Gtc(1, Side::Sell, 101, 5));
    book.AddOrder(Gtc(2, Side::Sell, 100, 5));
    book.AddOrder(Gtc(3, Side::Sell, 100, 5));
    book.AddOrder(Gtc(4, Side::Sell, 102, 5));  // out of the buyer's range

    const auto trades = book.AddOrder(Gtc(5, Side::Buy, 101, 12));
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
        book.AddOrder(Gtc(id, Side::Buy, 100, 1));
    const auto trades = book.AddOrder(Gtc(1000, Side::Sell, 100, 100));
    ASSERT_EQ(trades.size(), 100u);
    for (OrderId id = 1; id <= 100; ++id)
        ExpectTrade(trades[id - 1], id, 1000, 100, 1);
    EXPECT_EQ(book.Size(), 0u);
}

// ---------- FillAndKill ----------

TEST(OrderBook, FillAndKillOnEmptyBookRejected) {
    OrderBook book;
    EXPECT_TRUE(book.AddOrder(Fak(1, Side::Buy, 100, 10)).empty());
    EXPECT_EQ(book.Size(), 0u);
}

TEST(OrderBook, FillAndKillWithoutCrossRejected) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Buy, 100, 10));
    EXPECT_TRUE(book.AddOrder(Fak(2, Side::Sell, 101, 10)).empty());
    EXPECT_EQ(book.Size(), 1u);
    EXPECT_TRUE(book.GetOrderInfos().GetAsks().empty());
}

TEST(OrderBook, FillAndKillRemainderCancelled) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Sell, 100, 5));
    const auto trades = book.AddOrder(Fak(2, Side::Buy, 100, 8));
    ASSERT_EQ(trades.size(), 1u);
    ExpectTrade(trades[0], 2, 1, 100, 5);
    EXPECT_EQ(book.Size(), 0u);
    EXPECT_TRUE(book.GetOrderInfos().GetBids().empty());
}

TEST(OrderBook, FillAndKillSweepsThenKillsRemainder) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Buy, 100, 5));
    book.AddOrder(Gtc(2, Side::Buy, 99, 5));
    book.AddOrder(Gtc(3, Side::Buy, 98, 5));
    const auto trades = book.AddOrder(Fak(4, Side::Sell, 99, 20));
    ASSERT_EQ(trades.size(), 2u);
    ExpectTrade(trades[0], 1, 4, 100, 5);
    ExpectTrade(trades[1], 2, 4, 99, 5);
    EXPECT_EQ(book.Size(), 1u);  // only #3 remains
    const auto infos = book.GetOrderInfos();
    EXPECT_TRUE(infos.GetAsks().empty());
    ASSERT_EQ(infos.GetBids().size(), 1u);
    ExpectLevel(infos.GetBids()[0], 98, 5);
}

TEST(OrderBook, FillAndKillFullyFilledDoesNotRest) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Sell, 100, 10));
    const auto trades = book.AddOrder(Fak(2, Side::Buy, 100, 4));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(book.Size(), 1u);
    ExpectLevel(book.GetOrderInfos().GetAsks()[0], 100, 6);
}

// ---------- Cancel ----------

TEST(OrderBook, CancelRemovesOrderAndEmptyLevel) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Buy, 100, 10));
    book.AddOrder(Gtc(2, Side::Sell, 101, 10));
    book.CancelOrder(1);
    EXPECT_EQ(book.Size(), 1u);
    EXPECT_TRUE(book.GetOrderInfos().GetBids().empty());
    book.CancelOrder(2);
    EXPECT_EQ(book.Size(), 0u);
    EXPECT_TRUE(book.GetOrderInfos().GetAsks().empty());
}

TEST(OrderBook, CancelUnknownOrTwiceIsNoOp) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Buy, 100, 10));
    book.CancelOrder(42);
    EXPECT_EQ(book.Size(), 1u);
    book.CancelOrder(1);
    book.CancelOrder(1);
    EXPECT_EQ(book.Size(), 0u);
}

TEST(OrderBook, CancelMiddleOfLevelKeepsFifoOfOthers) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Sell, 100, 3));
    book.AddOrder(Gtc(2, Side::Sell, 100, 3));
    book.AddOrder(Gtc(3, Side::Sell, 100, 3));
    book.CancelOrder(2);
    ExpectLevel(book.GetOrderInfos().GetAsks()[0], 100, 6);

    const auto trades = book.AddOrder(Gtc(4, Side::Buy, 100, 6));
    ASSERT_EQ(trades.size(), 2u);
    ExpectTrade(trades[0], 4, 1, 100, 3);
    ExpectTrade(trades[1], 4, 3, 100, 3);
    EXPECT_EQ(book.Size(), 0u);
}

TEST(OrderBook, CancelFilledOrderIsNoOp) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Sell, 100, 5));
    book.AddOrder(Gtc(2, Side::Buy, 100, 5));
    book.CancelOrder(1);
    book.CancelOrder(2);
    EXPECT_EQ(book.Size(), 0u);
}

// ---------- Modify ----------

TEST(OrderBook, ModifyUnknownIsNoOp) {
    OrderBook book;
    EXPECT_TRUE(book.ModifyOrder(OrderModify{1, Side::Buy, 10, 100}).empty());
    EXPECT_EQ(book.Size(), 0u);
}

TEST(OrderBook, ModifyChangesPriceAndQuantity) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Buy, 100, 10));
    EXPECT_TRUE(book.ModifyOrder(OrderModify{1, Side::Buy, 25, 98}).empty());
    EXPECT_EQ(book.Size(), 1u);
    const auto infos = book.GetOrderInfos();
    ASSERT_EQ(infos.GetBids().size(), 1u);
    ExpectLevel(infos.GetBids()[0], 98, 25);
}

TEST(OrderBook, ModifyLosesTimePriority) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Buy, 100, 5));
    book.AddOrder(Gtc(2, Side::Buy, 100, 5));
    book.ModifyOrder(OrderModify{1, Side::Buy, 5, 100});  // #1 goes to the back

    const auto trades = book.AddOrder(Gtc(3, Side::Sell, 100, 5));
    ASSERT_EQ(trades.size(), 1u);
    ExpectTrade(trades[0], 2, 3, 100, 5);
}

TEST(OrderBook, ModifyCanTriggerMatch) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Buy, 99, 5));
    book.AddOrder(Gtc(2, Side::Sell, 101, 5));
    const auto trades = book.ModifyOrder(OrderModify{1, Side::Buy, 5, 102});
    ASSERT_EQ(trades.size(), 1u);
    ExpectTrade(trades[0], 1, 2, 101, 5);  // maker (#2) price
    EXPECT_EQ(book.Size(), 0u);
}

TEST(OrderBook, ModifyPreservesOrderType) {
    OrderBook book;
    book.AddOrder(Gtc(1, Side::Sell, 101, 5));
    book.AddOrder(Gtc(2, Side::Buy, 99, 5));
    // Modifying a GTC to a non-crossing price keeps it resting (it stays GTC).
    EXPECT_TRUE(book.ModifyOrder(OrderModify{2, Side::Buy, 5, 100}).empty());
    EXPECT_EQ(book.Size(), 2u);
}
