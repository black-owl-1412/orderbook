#pragma once

// Live simulation model behind the viewer, kept free of any UI code.
//
// Each Step() draws one event from the Hawkes order-flow generator and applies it to
// the real engine and to the reference book (the generator reads the reference
// book to pick cancel targets and size sweeps). A mismatch between the two is
// flagged, so the viewer doubles as a live sanity check.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

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
        events_ = trades_ = rejects_ = 0;
        rejectsByReason_ = {};
        lastTradePrice_.reset();
        mismatch_ = false;
    }

    void Step() {
        State& s = *state_;
        const sim::Event e = s.gen.Next(s.ref);
        ++events_;

        if (e.type == sim::EventType::Cancel) {
            const auto real = s.book.CancelOrder(e.id);
            const auto expected = s.ref.Cancel(e.id);
            if (real.has_value() != expected.has_value())
                mismatch_ = true;
            if (!real)
                CountReject(real.error());
        } else {
            const Side side = e.IsBuy() ? Side::Buy : Side::Sell;
            const OrderType type = e.IsMarket() ? OrderType::Market : OrderType::GoodTillCancel;
            const auto real = e.IsMarket() ? s.book.AddOrder(Order{e.id, side, e.qty})
                                           : s.book.AddOrder(Order{type, e.id, side, e.qty, e.price});
            const auto expected = s.ref.Add(type, e.id, side, e.price, e.qty);
            if (real.has_value() != expected.has_value() ||
                (real && real->size() != expected->size()))
                mismatch_ = true;
            if (!real) {
                CountReject(real.error());
            } else if (!real->empty()) {
                trades_ += real->size();
                lastTradePrice_ = real->back().GetPrice();
            }
        }

        if (s.book.Size() != s.ref.Size() || s.book.BestBid() != s.ref.BestBid() ||
            s.book.BestAsk() != s.ref.BestAsk())
            mismatch_ = true;
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

private:
    struct State {
        State(sim::FlowConfig cfg, std::uint64_t seed) : gen{std::move(cfg), seed} {}
        OrderBook book;
        sim::ReferenceBook ref;
        sim::OrderFlowGenerator gen;
    };

    void CountReject(RejectReason r) {
        ++rejects_;
        ++rejectsByReason_[static_cast<std::size_t>(r)];
    }

    std::optional<State> state_;
    std::uint64_t events_ = 0;
    std::uint64_t trades_ = 0;
    std::uint64_t rejects_ = 0;
    std::array<std::uint64_t, kRejectReasons> rejectsByReason_{};
    std::optional<Price> lastTradePrice_;
    bool mismatch_ = false;
};

}  // namespace viz
