#pragma once

// Live simulation model behind the viewer, kept free of any UI code.
//
// Each Step() draws one event from the Hawkes order-flow generator and applies it to
// the real engine and to the reference book (the generator reads the reference
// book to pick cancel targets and size sweeps). A mismatch between the two is
// flagged, so the viewer doubles as a live sanity check.
//
// The user can also place their own orders (PlaceUserOrder). They take exactly the
// same path as simulated orders, both books and the same checks, and every trade
// is credited to the user order on either side of it, so a resting user order that
// is later hit by simulated flow shows up as filled.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "orderbook/OrderBook.h"
#include "orderbook/ToString.h"
#include "sim/OrderFlow.h"
#include "sim/ReferenceBook.h"
#include "sim/Scenarios.h"

namespace viz {

using namespace orderbook;

struct Scenario {
    std::string_view name;
    sim::FlowConfig (*make)();
};

inline constexpr std::array kScenarios{
    Scenario{"Poisson", &sim::scenarios::Poisson},
    Scenario{"Calm", &sim::scenarios::Calm},
    Scenario{"Volatile", &sim::scenarios::Volatile},
    Scenario{"Crash", &sim::scenarios::Crash},
    Scenario{"Sweep", &sim::scenarios::Sweep},
};

inline constexpr std::size_t kRejectReasons = 6;  // RejectReason enumerators

// User order ids live far above the generator's (which start at 1, with never-existed
// cancel targets near 1e15), so the two can never collide.
inline constexpr OrderId kUserIdBase = 9'000'000'000'000'000'000ull;

enum class UserStatus : std::uint8_t {
    Resting,       // in the book, nothing filled yet
    PartlyFilled,  // in the book, some filled
    Filled,        // completely filled
    Cancelled,     // cancelled by the user, or an IOC remainder that was killed
    Expired,       // GoodForDay order removed at end of day
    Rejected,      // refused by the engine; see reject
};

constexpr std::string_view ToString(UserStatus s) {
    switch (s) {
        case UserStatus::Resting: return "resting";
        case UserStatus::PartlyFilled: return "partly filled";
        case UserStatus::Filled: return "filled";
        case UserStatus::Cancelled: return "cancelled";
        case UserStatus::Expired: return "expired";
        case UserStatus::Rejected: return "rejected";
    }
    std::unreachable();
}

struct UserOrder {
    OrderId id;
    OrderType type;
    Side side;
    Price price;  // as entered; ignored for Market orders
    Quantity qty;
    Quantity filled = 0;
    UserStatus status = UserStatus::Resting;
    std::optional<RejectReason> reject;
    Price lastFillPrice = 0;  // valid when filled > 0

    bool Live() const { return status == UserStatus::Resting || status == UserStatus::PartlyFilled; }
    Quantity Remaining() const { return qty - filled; }
};

class LiveSim {
public:
    // stableBook: apply sim::scenarios::StableBook so the book stays deep; false runs
    // the regime exactly as the market-simulation tests do.
    LiveSim(std::size_t scenario, std::uint64_t seed, bool stableBook) {
        Reset(scenario, seed, stableBook);
    }

    void Reset(std::size_t scenario, std::uint64_t seed, bool stableBook) {
        sim::FlowConfig cfg = kScenarios.at(scenario).make();
        if (stableBook)
            cfg = sim::scenarios::StableBook(std::move(cfg));
        state_.reset();  // destroy the old book before building the new one
        state_.emplace(std::move(cfg), seed);
        events_ = trades_ = rejects_ = skippedUserCancels_ = 0;
        rejectsByReason_ = {};
        lastTradePrice_.reset();
        mismatch_ = false;
        userOrders_.clear();
        userIndex_.clear();
        userResting_.clear();
        nextUserId_ = kUserIdBase;
    }

    // One simulated event.
    void Step() {
        State& s = *state_;
        const sim::Event e = s.gen.Next(s.ref);
        ++events_;

        if (e.type == sim::EventType::Cancel) {
            // The generator picks cancel targets among all live orders, which includes
            // the user's. Only the user may cancel their orders: skip the event on both
            // books so they stay identical.
            if (const UserOrder* u = MutableUser(e.id); u && u->Live()) {
                ++skippedUserCancels_;
            } else {
                const auto real = s.book.CancelOrder(e.id);
                const auto expected = s.ref.Cancel(e.id);
                if (real.has_value() != expected.has_value())
                    mismatch_ = true;
                if (!real)
                    CountReject(real.error());
            }
        } else {
            const Side side = e.IsBuy() ? Side::Buy : Side::Sell;
            const OrderType type = e.IsMarket() ? OrderType::Market : OrderType::GoodTillCancel;
            (void)ApplyAdd(type, e.id, side, e.price, e.qty);
        }
        CheckTopOfBook();
    }

    // Sends a user order to the book. It matches immediately (running or paused); any
    // remainder of a GTC/GFD order rests and can be filled later by simulated flow.
    AddResult PlaceUserOrder(OrderType type, Side side, Price price, Quantity qty) {
        const OrderId id = nextUserId_++;
        userIndex_.emplace(id, userOrders_.size());
        userOrders_.push_back(UserOrder{id, type, side, price, qty, 0, UserStatus::Resting, std::nullopt, 0});

        placingId_ = id;  // fills credited during placement don't touch the resting totals
        const AddResult result = ApplyAdd(type, id, side, price, qty);
        placingId_.reset();

        UserOrder& u = userOrders_.back();
        if (!result) {
            u.status = UserStatus::Rejected;
            u.reject = result.error();
        } else if (u.filled == u.qty) {
            u.status = UserStatus::Filled;
        } else if (IsImmediateOrCancel(type)) {
            u.status = UserStatus::Cancelled;  // the unfilled rest was killed
        } else {
            u.status = u.filled > 0 ? UserStatus::PartlyFilled : UserStatus::Resting;
            userResting_[{side, price}] += u.Remaining();
        }
        CheckTopOfBook();
        return result;
    }

    // Cancels a live user order. False if the id is not a live user order.
    bool CancelUserOrder(OrderId id) {
        UserOrder* u = MutableUser(id);
        if (!u || !u->Live())
            return false;
        State& s = *state_;
        const auto real = s.book.CancelOrder(id);
        const auto expected = s.ref.Cancel(id);
        if (!real || !expected)
            mismatch_ = true;  // a live user order must be cancellable in both books
        RemoveResting(*u, u->Remaining());
        u->status = UserStatus::Cancelled;
        CheckTopOfBook();
        return true;
    }

    // End of trading day: every GoodForDay order in the book (user or not) expires.
    std::vector<OrderId> EndOfDay() {
        State& s = *state_;
        std::vector<OrderId> ids = s.book.CancelGoodForDayOrders();
        if (ids != s.ref.EndOfDay())
            mismatch_ = true;
        for (const OrderId id : ids)
            if (UserOrder* u = MutableUser(id)) {
                RemoveResting(*u, u->Remaining());
                u->status = UserStatus::Expired;
            }
        CheckTopOfBook();
        return ids;
    }

    const OrderBook& Book() const { return state_->book; }
    std::uint64_t Events() const { return events_; }
    std::uint64_t Trades() const { return trades_; }
    std::uint64_t Rejects() const { return rejects_; }
    std::uint64_t Rejects(RejectReason r) const {
        return rejectsByReason_[static_cast<std::size_t>(r)];
    }
    std::optional<Price> LastTradePrice() const { return lastTradePrice_; }
    bool Mismatch() const { return mismatch_; }

    // User orders, oldest first.
    const std::vector<UserOrder>& UserOrders() const { return userOrders_; }
    const UserOrder* FindUser(OrderId id) const {
        const auto it = userIndex_.find(id);
        return it == userIndex_.end() ? nullptr : &userOrders_[it->second];
    }
    // The user's total resting quantity at one price on one side (for the ladder).
    Quantity UserRestingAt(Side side, Price price) const {
        const auto it = userResting_.find({side, price});
        return it == userResting_.end() ? 0 : it->second;
    }
    // Simulated cancels that targeted a live user order and were skipped.
    std::uint64_t SkippedUserCancels() const { return skippedUserCancels_; }

private:
    struct State {
        State(sim::FlowConfig cfg, std::uint64_t seed) : gen{std::move(cfg), seed} {}
        OrderBook book;
        sim::ReferenceBook ref;
        sim::OrderFlowGenerator gen;
    };

    // Mutable lookup; a different name so the public const FindUser is never shadowed.
    UserOrder* MutableUser(OrderId id) {
        const auto it = userIndex_.find(id);
        return it == userIndex_.end() ? nullptr : &userOrders_[it->second];
    }

    // The single path for every new order, simulated or user: both books, compared
    // trade by trade; counters; fills credited to user orders on either leg.
    AddResult ApplyAdd(OrderType type, OrderId id, Side side, Price price, Quantity qty) {
        State& s = *state_;
        AddResult real = s.book.AddOrder(Order{type, id, side, qty, price});
        const AddResult expected = s.ref.Add(type, id, side, price, qty);
        if (!SameResult(real, expected))
            mismatch_ = true;
        if (!real) {
            CountReject(real.error());
            return real;
        }
        if (!real->empty()) {
            trades_ += real->size();
            lastTradePrice_ = real->back().GetPrice();
        }
        if (!userIndex_.empty())
            for (const Trade& t : *real) {
                CreditFill(t.GetBidTrade().orderId_, t.GetQuantity(), t.GetPrice());
                CreditFill(t.GetAskTrade().orderId_, t.GetQuantity(), t.GetPrice());
            }
        return real;
    }

    void CreditFill(OrderId id, Quantity qty, Price price) {
        UserOrder* u = MutableUser(id);
        if (!u)
            return;
        u->filled += qty;
        u->lastFillPrice = price;
        if (placingId_ == id)
            return;  // still being placed: PlaceUserOrder sets the final status
        RemoveResting(*u, qty);
        u->status = u->filled == u->qty ? UserStatus::Filled : UserStatus::PartlyFilled;
    }

    void RemoveResting(const UserOrder& u, Quantity qty) {
        const auto it = userResting_.find({u.side, u.price});
        if (it == userResting_.end())
            return;
        it->second -= std::min(it->second, qty);
        if (it->second == 0)
            userResting_.erase(it);
    }

    static bool SameResult(const AddResult& a, const AddResult& b) {
        if (a.has_value() != b.has_value())
            return false;
        if (!a)
            return a.error() == b.error();
        if (a->size() != b->size())
            return false;
        for (std::size_t i = 0; i < a->size(); ++i) {
            const Trade& x = (*a)[i];
            const Trade& y = (*b)[i];
            if (x.GetBidTrade().orderId_ != y.GetBidTrade().orderId_ ||
                x.GetAskTrade().orderId_ != y.GetAskTrade().orderId_ || x.GetPrice() != y.GetPrice() ||
                x.GetQuantity() != y.GetQuantity())
                return false;
        }
        return true;
    }

    void CheckTopOfBook() {
        const State& s = *state_;
        if (s.book.Size() != s.ref.Size() || s.book.BestBid() != s.ref.BestBid() ||
            s.book.BestAsk() != s.ref.BestAsk())
            mismatch_ = true;
    }

    void CountReject(RejectReason r) {
        ++rejects_;
        ++rejectsByReason_[static_cast<std::size_t>(r)];
    }

    std::optional<State> state_;
    std::uint64_t events_ = 0;
    std::uint64_t trades_ = 0;
    std::uint64_t rejects_ = 0;
    std::uint64_t skippedUserCancels_ = 0;
    std::array<std::uint64_t, kRejectReasons> rejectsByReason_{};
    std::optional<Price> lastTradePrice_;
    bool mismatch_ = false;

    std::vector<UserOrder> userOrders_;
    std::unordered_map<OrderId, std::size_t> userIndex_;
    std::map<std::pair<Side, Price>, Quantity> userResting_;
    OrderId nextUserId_ = kUserIdBase;
    std::optional<OrderId> placingId_;
};

}  // namespace viz
