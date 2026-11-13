// User orders in the live viewer's model (apps/viz/LiveSim.h): they go through the same
// engine + reference-book path as simulated flow, are credited with every fill on
// either side of a trade, and are never cancelled by the simulation.

#include <gtest/gtest.h>

#include <cstdint>

#include "LiveSim.h"

using namespace orderbook;
using viz::LiveSim;
using viz::UserOrder;
using viz::UserStatus;

namespace {

constexpr std::size_t kCalm = 1;  // index in viz::kScenarios

// Advances a simulation until both sides have depth. (LiveSim is not movable: the
// engine's object pool pins its storage, so tests build it in place.)
void WarmUp(LiveSim& sim) {
    for (int i = 0; i < 2'000; ++i)
        sim.Step();
    ASSERT_TRUE(sim.Book().BestBid() && sim.Book().BestAsk());
}

const UserOrder& Last(const LiveSim& sim) { return sim.UserOrders().back(); }

}  // namespace

TEST(LiveSimUser, CrossingLimitFillsAtRestingPrice) {
    LiveSim sim{kCalm, 1, true};
    WarmUp(sim);
    const Price ask = *sim.Book().BestAsk();
    const auto result = sim.PlaceUserOrder(OrderType::GoodTillCancel, Side::Buy, ask, 1);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ(result->front().GetPrice(), ask);
    EXPECT_GE(Last(sim).id, viz::kUserIdBase);
    EXPECT_EQ(Last(sim).status, UserStatus::Filled);
    EXPECT_EQ(Last(sim).filled, 1u);
    EXPECT_EQ(Last(sim).lastFillPrice, ask);
    EXPECT_EQ(sim.UserRestingAt(Side::Buy, ask), 0u);
    EXPECT_EQ(*sim.LastTradePrice(), ask);
    EXPECT_FALSE(sim.Mismatch());
}

TEST(LiveSimUser, NonCrossingLimitRestsAndIsShownAtItsPrice) {
    LiveSim sim{kCalm, 1, true};
    WarmUp(sim);
    const Price price = *sim.Book().BestBid() - 3;
    const std::size_t before = sim.Book().Size();
    ASSERT_TRUE(sim.PlaceUserOrder(OrderType::GoodTillCancel, Side::Buy, price, 7).has_value());
    EXPECT_EQ(Last(sim).status, UserStatus::Resting);
    EXPECT_EQ(sim.Book().Size(), before + 1);
    EXPECT_EQ(sim.UserRestingAt(Side::Buy, price), 7u);
    EXPECT_FALSE(sim.Mismatch());
}

TEST(LiveSimUser, RestingOrderIsFilledLaterBySimulatedFlow) {
    LiveSim sim{kCalm, 1, true};
    WarmUp(sim);
    const Price bid = *sim.Book().BestBid();  // joins the best bid queue
    ASSERT_TRUE(sim.PlaceUserOrder(OrderType::GoodTillCancel, Side::Buy, bid, 5).has_value());
    const OrderId id = Last(sim).id;

    bool sawPartial = false;
    for (int i = 0; i < 200'000 && sim.FindUser(id)->status != UserStatus::Filled; ++i) {
        sim.Step();
        sawPartial |= sim.FindUser(id)->status == UserStatus::PartlyFilled;
    }
    const UserOrder& u = *sim.FindUser(id);
    EXPECT_EQ(u.status, UserStatus::Filled);
    EXPECT_EQ(u.filled, 5u);
    EXPECT_LE(u.lastFillPrice, bid);  // a buy never fills above its limit
    EXPECT_EQ(sim.UserRestingAt(Side::Buy, bid), 0u);
    EXPECT_FALSE(sim.Mismatch());
    (void)sawPartial;  // depends on simulated sizes; filling completely is what matters
}

TEST(LiveSimUser, SimulationNeverCancelsUserOrders) {
    LiveSim sim{kCalm, 3, true};
    WarmUp(sim);
    // Far from the market so they are not filled; they stay live the whole run.
    const Price bid = *sim.Book().BestBid() - 40;
    const Price ask = *sim.Book().BestAsk() + 40;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(sim.PlaceUserOrder(OrderType::GoodTillCancel, Side::Buy, bid - i, 3).has_value());
        ASSERT_TRUE(sim.PlaceUserOrder(OrderType::GoodTillCancel, Side::Sell, ask + i, 3).has_value());
    }
    for (int i = 0; i < 50'000; ++i)
        sim.Step();

    EXPECT_GT(sim.SkippedUserCancels(), 0u) << "the generator should have targeted user orders";
    for (const UserOrder& u : sim.UserOrders())
        EXPECT_TRUE(u.status == UserStatus::Resting || u.status == UserStatus::PartlyFilled ||
                    u.status == UserStatus::Filled)
            << "user order #" << u.id << " ended " << viz::ToString(u.status);
    EXPECT_FALSE(sim.Mismatch());
}

TEST(LiveSimUser, ImmediateOrCancelRemaindersAreCancelled) {
    LiveSim sim{kCalm, 1, true};
    WarmUp(sim);
    const Price ask = *sim.Book().BestAsk();
    // FAK at the best ask for more than the whole book: fills what is there, kills the rest.
    ASSERT_TRUE(sim.PlaceUserOrder(OrderType::FillAndKill, Side::Buy, ask, 1'000'000).has_value());
    EXPECT_EQ(Last(sim).status, UserStatus::Cancelled);
    EXPECT_GT(Last(sim).filled, 0u);

    ASSERT_TRUE(sim.PlaceUserOrder(OrderType::Market, Side::Sell, 0, 1'000'000).has_value());
    EXPECT_EQ(Last(sim).status, UserStatus::Cancelled);
    EXPECT_GT(Last(sim).filled, 0u);
    EXPECT_EQ(sim.UserRestingAt(Side::Buy, ask), 0u);
    EXPECT_FALSE(sim.Mismatch());
}

TEST(LiveSimUser, RejectsCarryTheReason) {
    LiveSim sim{kCalm, 1, true};
    WarmUp(sim);
    const auto fok = sim.PlaceUserOrder(OrderType::FillOrKill, Side::Buy, *sim.Book().BestAsk(), 1'000'000);
    ASSERT_FALSE(fok.has_value());
    EXPECT_EQ(Last(sim).status, UserStatus::Rejected);
    EXPECT_EQ(Last(sim).reject, RejectReason::CannotFullyFill);

    ASSERT_FALSE(sim.PlaceUserOrder(OrderType::GoodTillCancel, Side::Buy, 100, 0).has_value());
    EXPECT_EQ(Last(sim).reject, RejectReason::ZeroQuantity);
    EXPECT_EQ(Last(sim).filled, 0u);
    EXPECT_FALSE(sim.Mismatch());
}

TEST(LiveSimUser, CancelRemovesTheOrderOnce) {
    LiveSim sim{kCalm, 1, true};
    WarmUp(sim);
    const Price price = *sim.Book().BestBid() - 5;
    ASSERT_TRUE(sim.PlaceUserOrder(OrderType::GoodTillCancel, Side::Buy, price, 4).has_value());
    const OrderId id = Last(sim).id;
    const std::size_t before = sim.Book().Size();

    EXPECT_TRUE(sim.CancelUserOrder(id));
    EXPECT_EQ(sim.FindUser(id)->status, UserStatus::Cancelled);
    EXPECT_EQ(sim.Book().Size(), before - 1);
    EXPECT_EQ(sim.UserRestingAt(Side::Buy, price), 0u);
    EXPECT_FALSE(sim.CancelUserOrder(id)) << "already cancelled";
    EXPECT_FALSE(sim.CancelUserOrder(12345)) << "not a user order";
    EXPECT_FALSE(sim.Mismatch());
}

TEST(LiveSimUser, EndOfDayExpiresGoodForDayOnly) {
    LiveSim sim{kCalm, 1, true};
    WarmUp(sim);
    const Price price = *sim.Book().BestBid() - 5;
    ASSERT_TRUE(sim.PlaceUserOrder(OrderType::GoodForDay, Side::Buy, price, 2).has_value());
    const OrderId gfd = Last(sim).id;
    ASSERT_TRUE(sim.PlaceUserOrder(OrderType::GoodTillCancel, Side::Buy, price, 3).has_value());
    const OrderId gtc = Last(sim).id;

    const auto expired = sim.EndOfDay();
    ASSERT_EQ(expired.size(), 1u);  // the simulation only sends GTC, so just ours
    EXPECT_EQ(expired.front(), gfd);
    EXPECT_EQ(sim.FindUser(gfd)->status, UserStatus::Expired);
    EXPECT_EQ(sim.FindUser(gtc)->status, UserStatus::Resting);
    EXPECT_EQ(sim.UserRestingAt(Side::Buy, price), 3u);
    EXPECT_FALSE(sim.Mismatch());
}

TEST(LiveSimUser, ResetForgetsUserOrders) {
    LiveSim sim{kCalm, 1, true};
    WarmUp(sim);
    ASSERT_TRUE(sim.PlaceUserOrder(OrderType::GoodTillCancel, Side::Buy, *sim.Book().BestBid() - 2, 1));
    sim.Reset(kCalm, 1, true);
    EXPECT_TRUE(sim.UserOrders().empty());
    ASSERT_TRUE(sim.PlaceUserOrder(OrderType::GoodTillCancel, Side::Buy, 9'000, 1).has_value());
    EXPECT_EQ(Last(sim).id, viz::kUserIdBase) << "ids restart after a reset";
}
