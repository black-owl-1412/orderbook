#pragma once

// Runs one scenario: generates events, feeds them to the real engine, the
// reference model and the invariant ledger, and stops at the FIRST discrepancy.
// The stream up to and including that event is the shortest failing prefix; it is
// written to a file so it can be inspected and replayed.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "Ledger.h"
#include "OrderFlow.h"
#include "ReferenceBook.h"

namespace sim {

struct RunOptions {
    std::string name = "scenario";
    std::size_t events = 20'000;
    std::size_t levelCheckEvery = 1;      // zero-qty levels + conservation (O(levels))
    std::size_t levelTotalsEvery = 100;   // per-level totals vs ledger (O(orders))
    std::size_t depthCompareEvery = 100;  // full depth vs reference (O(orders))
    std::size_t depthSampleEvery = 50;    // statistics: depth profile sampling
    std::size_t depthProfileTicks = 40;
    double midSampleInterval = 1.0;       // statistics: book mid sampled on this time grid
};

struct RunStats {
    std::vector<double> times;           // event timestamps
    std::vector<double> aggressorSigns;  // +1/-1 for every event that produced trades
    std::vector<double> sizes;           // sizes of submitted limit/market orders
    std::vector<double> mids;            // book mid sampled every midSampleInterval
    std::vector<double> depthProfile;    // mean resting qty at distance d ticks from mid
    std::size_t trades = 0;
    std::size_t rejects = 0;
    std::size_t maxLevelsSwept = 0;      // distinct prices hit by one event
    std::size_t tiedTimestamps = 0;      // events sharing the previous event's timestamp
    std::size_t longestQueue = 0;        // longest FIFO queue seen at one price
    std::size_t sweeps = 0;
    std::size_t badCancels = 0;
    std::size_t restingAtEnd = 0;
    double startMid = 0, endMid = 0;
};

struct Failure {
    std::size_t index;  // failing event; shortest failing prefix length = index + 1
    std::string what;
    std::string prefixFile;
};

struct RunResult {
    std::optional<Failure> failure;
    std::vector<Event> stream;
    RunStats stats;
};

inline std::string FormatFailure(const RunResult& r) {
    if (!r.failure)
        return "no failure";
    const Failure& f = *r.failure;
    std::string s = std::format(
        "FIRST FAILURE at event {} => shortest failing prefix = {} events (saved to {})\n  {}\n"
        "  last events:\n",
        f.index, f.index + 1, f.prefixFile, f.what);
    const std::size_t from = f.index >= 15 ? f.index - 15 : 0;
    for (std::size_t i = from; i <= f.index; ++i)
        s += "    " + Describe(r.stream[i]) + "\n";
    return s;
}

namespace detail {

inline std::optional<std::string> CompareAdd(const AddResult& real, const AddResult& ref) {
    if (real.has_value() != ref.has_value())
        return std::format("engine {} but reference {}",
                           real ? "accepted" : std::string{"rejected "} + std::string{ToString(real.error())},
                           ref ? "accepted" : std::string{"rejected "} + std::string{ToString(ref.error())});
    if (!real) {
        if (real.error() != ref.error())
            return std::format("reject reason {} vs reference {}", ToString(real.error()),
                               ToString(ref.error()));
        return std::nullopt;
    }
    if (real->size() != ref->size())
        return std::format("{} trades vs reference {}", real->size(), ref->size());
    for (std::size_t i = 0; i < real->size(); ++i) {
        const Trade& a = (*real)[i];
        const Trade& b = (*ref)[i];
        if (a.GetBidTrade().orderId_ != b.GetBidTrade().orderId_ ||
            a.GetAskTrade().orderId_ != b.GetAskTrade().orderId_ || a.GetPrice() != b.GetPrice() ||
            a.GetQuantity() != b.GetQuantity())
            return std::format("trade {} differs: engine bid#{} ask#{} {}@{} vs reference bid#{} ask#{} {}@{}",
                               i, a.GetBidTrade().orderId_, a.GetAskTrade().orderId_,
                               a.GetQuantity(), a.GetPrice(), b.GetBidTrade().orderId_,
                               b.GetAskTrade().orderId_, b.GetQuantity(), b.GetPrice());
    }
    return std::nullopt;
}

inline std::optional<std::string> CompareDepth(const OrderBook& real, const ReferenceBook& ref) {
    const auto a = real.GetOrderInfos();
    const auto b = ref.Depth();
    for (int s = 0; s < 2; ++s) {
        const LevelInfos& x = s == 0 ? a.GetBids() : a.GetAsks();
        const LevelInfos& y = s == 0 ? b.GetBids() : b.GetAsks();
        if (x.size() != y.size())
            return std::format("{} level count {} vs reference {}", s == 0 ? "bid" : "ask",
                               x.size(), y.size());
        for (std::size_t i = 0; i < x.size(); ++i)
            if (x[i].price_ != y[i].price_ || x[i].quantity_ != y[i].quantity_)
                return std::format("{} level {}: {}@{} vs reference {}@{}", s == 0 ? "bid" : "ask",
                                   i, x[i].quantity_, x[i].price_, y[i].quantity_, y[i].price_);
    }
    return std::nullopt;
}

inline std::string WritePrefix(const std::string& name, const std::vector<Event>& stream,
                               std::size_t lastIndex) {
    const auto path = std::filesystem::current_path() / std::format("failing_prefix_{}.txt", name);
    std::ofstream out{path};
    for (std::size_t i = 0; i <= lastIndex; ++i)
        out << Describe(stream[i]) << '\n';
    return path.string();
}

}  // namespace detail

// Applies identical seed orders to all three participants before the flow starts.
class Seeder {
public:
    Seeder(OrderBook& book, ReferenceBook& ref, Ledger& ledger)
        : book_{book}, ref_{ref}, ledger_{ledger} {}

    std::optional<std::string> Add(Side side, Price price, Quantity qty) {
        const OrderId id = nextId_++;
        const auto real = book_.AddOrder(Order{OrderType::GoodTillCancel, id, side, qty, price});
        const auto expected = ref_.Add(OrderType::GoodTillCancel, id, side, price, qty);
        if (auto d = detail::CompareAdd(real, expected))
            return "while seeding: " + *d;
        return ledger_.OnAdd(OrderType::GoodTillCancel, id, side, price, qty, real);
    }

private:
    OrderBook& book_;
    ReferenceBook& ref_;
    Ledger& ledger_;
    OrderId nextId_ = 5'000'000'000'000'000'000ull;  // far from generator ids
};

using Prefill = std::function<std::optional<std::string>(Seeder&)>;

inline RunResult RunScenario(const FlowConfig& cfg, std::uint64_t seed, const RunOptions& opt,
                             const Prefill& prefill = {}) {
    RunResult result;
    RunStats& st = result.stats;
    OrderBook book;
    ReferenceBook ref;
    Ledger ledger;
    OrderFlowGenerator gen{cfg, seed};

    if (prefill) {
        Seeder seeder{book, ref, ledger};
        if (auto err = prefill(seeder)) {
            result.failure = Failure{0, *err, "(during prefill)"};
            return result;
        }
    }

    std::vector<double> depthSum(opt.depthProfileTicks + 1, 0.0);
    std::size_t depthSamples = 0;
    double nextMidSample = 0.0;
    st.startMid = gen.Mid();
    result.stream.reserve(opt.events);

    auto fail = [&](std::size_t i, std::string what) {
        result.failure = Failure{i, std::move(what), detail::WritePrefix(opt.name, result.stream, i)};
    };

    for (std::size_t i = 0; i < opt.events; ++i) {
        const Event e = gen.Next(ref);
        result.stream.push_back(e);
        if (i > 0 && e.time == result.stream[i - 1].time)
            ++st.tiedTimestamps;
        st.times.push_back(e.time);

        std::optional<std::string> problem;
        if (e.type == EventType::Cancel) {
            const auto real = book.CancelOrder(e.id);
            const auto expected = ref.Cancel(e.id);
            if (real.has_value() != expected.has_value())
                problem = std::format("cancel: engine {} reference {}", real.has_value(),
                                      expected.has_value());
            else
                problem = ledger.OnCancel(e.id, real);
        } else {
            const Side side = e.IsBuy() ? Side::Buy : Side::Sell;
            const OrderType type = e.IsMarket() ? OrderType::Market : OrderType::GoodTillCancel;
            st.sizes.push_back(e.qty);
            const auto real = e.IsMarket() ? book.AddOrder(Order{e.id, side, e.qty})
                                           : book.AddOrder(Order{type, e.id, side, e.qty, e.price});
            const auto expected = ref.Add(type, e.id, side, e.price, e.qty);
            problem = detail::CompareAdd(real, expected);
            if (!problem)
                problem = ledger.OnAdd(type, e.id, side, e.price, e.qty, real);
            if (!problem) {
                if (!real) {
                    ++st.rejects;
                } else if (!real->empty()) {
                    st.trades += real->size();
                    st.aggressorSigns.push_back(e.IsBuy() ? 1.0 : -1.0);
                    std::set<Price> prices;
                    for (const Trade& t : *real)
                        prices.insert(t.GetPrice());
                    st.maxLevelsSwept = std::max(st.maxLevelsSwept, prices.size());
                }
            }
        }

        if (!problem)
            problem = ledger.CheckTop(book);
        if (!problem && (book.Size() != ref.Size() || book.BestBid() != ref.BestBid() ||
                         book.BestAsk() != ref.BestAsk()))
            problem = std::format("top of book / size differs from reference");
        if (!problem && opt.levelCheckEvery && i % opt.levelCheckEvery == 0)
            problem = ledger.CheckLevels(book);
        if (!problem && opt.levelTotalsEvery && i % opt.levelTotalsEvery == 0)
            problem = ledger.CheckLevelTotals(book);
        if (!problem && opt.depthCompareEvery && i % opt.depthCompareEvery == 0)
            problem = detail::CompareDepth(book, ref);
        if (problem) {
            fail(i, *problem);
            st.restingAtEnd = book.Size();
            st.endMid = gen.Mid();
            return result;
        }

        // ---- statistics (reference book is trusted at this point) ----
        const auto bid = ref.BestBid();
        const auto ask = ref.BestAsk();
        if (bid && ask) {
            const double mid = 0.5 * (*bid + *ask);
            while (e.time >= nextMidSample) {
                st.mids.push_back(mid);
                nextMidSample += opt.midSampleInterval;
            }
            if (opt.depthSampleEvery && i % opt.depthSampleEvery == 0) {
                const auto depth = ref.Depth(opt.depthProfileTicks + 1);
                for (const LevelInfo& l : depth.GetBids())
                    if (const double d = mid - l.price_; d <= opt.depthProfileTicks)
                        depthSum[static_cast<std::size_t>(d)] += l.quantity_;
                for (const LevelInfo& l : depth.GetAsks())
                    if (const double d = l.price_ - mid; d <= opt.depthProfileTicks)
                        depthSum[static_cast<std::size_t>(d)] += l.quantity_;
                ++depthSamples;
            }
        }
        if (i % 1000 == 0)
            st.longestQueue = std::max(st.longestQueue, ledger.LongestQueue());
    }

    // Final full checks.
    std::optional<std::string> problem = ledger.CheckLevels(book);
    if (!problem) problem = ledger.CheckLevelTotals(book);
    if (!problem) problem = detail::CompareDepth(book, ref);
    if (problem && !result.stream.empty())
        fail(result.stream.size() - 1, "final check: " + *problem);

    for (double& d : depthSum)
        d = depthSamples ? d / static_cast<double>(depthSamples * 2) : 0.0;  // per side
    st.depthProfile = std::move(depthSum);
    st.longestQueue = std::max(st.longestQueue, ledger.LongestQueue());
    st.sweeps = gen.Sweeps();
    st.badCancels = gen.BadCancels();
    st.restingAtEnd = book.Size();
    st.endMid = gen.Mid();
    return result;
}

}  // namespace sim
