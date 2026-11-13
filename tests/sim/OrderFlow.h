#pragma once

// Stochastic order-flow generator.
//
//  * arrival times + event type: a 5-dimensional Hawkes process (Poisson if alpha == 0),
//    one dimension per event type
//  * limit price: mid -/+ d, d ~ Exponential or Lomax (power-law tail), on the tick grid
//  * size: log-normal, rounded, >= 1
//  * cancel: uniform over live orders, plus a small probability of targeting an
//    order that is already dead or never existed
//  * mid price: driven by order-flow imbalance + Gaussian noise
//      I    <- I * exp(-imbalanceDecay * dt) + sign * weight      (per event)
//      mid  <- mid + priceImpact * I + midNoise * N(0,1)
//
// Cancel targets and sweep sizes depend on the current book, so the generator
// reads it through the (trusted) ReferenceBook. Every produced Event is concrete,
// so the recorded stream can be replayed exactly.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>

#include "Hawkes.h"
#include "ReferenceBook.h"

namespace sim {

enum class EventType : std::uint8_t { BuyLimit, SellLimit, BuyMarket, SellMarket, Cancel };
inline constexpr std::size_t kEventTypes = 5;

constexpr std::string_view ToString(EventType t) {
    switch (t) {
        case EventType::BuyLimit: return "BuyLimit";
        case EventType::SellLimit: return "SellLimit";
        case EventType::BuyMarket: return "BuyMarket";
        case EventType::SellMarket: return "SellMarket";
        case EventType::Cancel: return "Cancel";
    }
    std::unreachable();
}

struct Event {
    std::uint64_t seq;
    double time;
    EventType type;
    OrderId id;  // new order id, or the cancel target
    Price price;
    Quantity qty;

    bool IsBuy() const { return type == EventType::BuyLimit || type == EventType::BuyMarket; }
    bool IsMarket() const { return type == EventType::BuyMarket || type == EventType::SellMarket; }
};

inline std::string Describe(const Event& e) {
    switch (e.type) {
        case EventType::BuyLimit:
        case EventType::SellLimit:
            return std::format("[{}] t={:.6f} {} #{} {} @ {}", e.seq, e.time, ToString(e.type), e.id,
                               e.qty, e.price);
        case EventType::BuyMarket:
        case EventType::SellMarket:
            return std::format("[{}] t={:.6f} {} #{} {}", e.seq, e.time, ToString(e.type), e.id,
                               e.qty);
        case EventType::Cancel:
            return std::format("[{}] t={:.6f} Cancel #{}", e.seq, e.time, e.id);
    }
    std::unreachable();
}

enum class DistanceLaw { Exponential, PowerLaw };

struct FlowConfig {
    HawkesParams arrivals;  // dimension kEventTypes, indexed by EventType

    double initialMid = 10'000.5;
    DistanceLaw distanceLaw = DistanceLaw::Exponential;
    double distanceMean = 3.0;     // Exponential: mean distance in ticks
    double powerLawShape = 1.5;    // Lomax tail exponent (smaller = heavier)
    double powerLawScale = 2.0;    // Lomax scale in ticks
    double maxDistance = 5'000.0;  // hard cap, ticks

    double sizeLogMean = 2.0;  // log-normal parameters of order size
    double sizeLogSigma = 1.0;
    Quantity maxSize = 1'000'000;

    double badCancelProb = 0.02;  // cancel a dead or never-existing id
    // > 0: on top of the Hawkes cancel stream, every resting order is cancelled at this
    // rate (per unit time), so the total cancel rate grows with the book. This gives the
    // book a stable size; with only a fixed-rate cancel stream its order count wanders
    // like a random walk and one side often runs empty. 0 keeps the original model.
    double cancelRatePerOrder = 0.0;

    double imbalanceDecay = 0.5;  // per unit time
    double marketWeight = 1.0;    // pressure contributed by one market order
    double limitWeight = 0.3;     // ... and by one limit order
    double priceImpact = 0.0;     // ticks per unit of pressure, per event
    double midNoise = 0.0;        // Gaussian ticks per event
    // In [0, 1]: after each event the generator's mid moves this fraction of the way to
    // the book's own mid (when both sides exist). Traders quote around the visible book,
    // so without this the generator's mid can drift away and new orders land off-centre.
    // 0 keeps the original model.
    double midReversion = 0.0;

    double sweepProb = 0.0;       // market order sized to sweep visible depth
    double sweepFactor = 1.5;     // multiple of the visible quantity
    std::size_t sweepLevels = 5;  // levels counted as "visible"

    double timestampResolution = 0.0;  // > 0: round times down => simultaneous events
};

// Helper: same alpha/beta for every (i, j) pair.
inline HawkesParams UniformHawkes(std::array<double, kEventTypes> mu, double branching,
                                  double beta) {
    HawkesParams p;
    p.mu.assign(mu.begin(), mu.end());
    p.alpha = Constant(kEventTypes, branching * beta / kEventTypes);
    p.beta = Constant(kEventTypes, beta);
    return p;
}

class OrderFlowGenerator {
public:
    OrderFlowGenerator(FlowConfig cfg, std::uint64_t seed)
        : cfg_{std::move(cfg)}, hawkes_{cfg_.arrivals, seed}, rng_{seed ^ 0x9E3779B97F4A7C15ull},
          mid_{cfg_.initialMid} {}

    Event Next(const ReferenceBook& book) {
        const auto arrival = NextArrival(book);
        double t = arrival.time;
        if (cfg_.timestampResolution > 0)
            t = std::floor(t / cfg_.timestampResolution) * cfg_.timestampResolution;
        imbalance_ *= std::exp(-cfg_.imbalanceDecay * (t - lastTime_));
        lastTime_ = t;

        Event e{seq_++, t, static_cast<EventType>(arrival.type), 0, 0, 0};
        const double sign = e.IsBuy() ? 1.0 : -1.0;
        switch (e.type) {
            case EventType::BuyLimit:
            case EventType::SellLimit: {
                const double d = DrawDistance();
                e.price = static_cast<Price>(e.IsBuy() ? std::floor(mid_ - d) : std::ceil(mid_ + d));
                e.qty = DrawSize();
                e.id = nextId_++;
                imbalance_ += sign * cfg_.limitWeight;
                break;
            }
            case EventType::BuyMarket:
            case EventType::SellMarket: {
                e.qty = DrawSize();
                if (cfg_.sweepProb > 0 && uniform_(rng_) < cfg_.sweepProb) {
                    const Side opposite = e.IsBuy() ? Side::Sell : Side::Buy;
                    const double visible =
                        static_cast<double>(book.VisibleQuantity(opposite, cfg_.sweepLevels));
                    e.qty = std::max(e.qty, static_cast<Quantity>(std::ceil(visible * cfg_.sweepFactor)));
                    ++sweeps_;
                }
                e.id = nextId_++;
                imbalance_ += sign * cfg_.marketWeight;
                break;
            }
            case EventType::Cancel:
                e.id = DrawCancelTarget(book);
                break;
        }
        mid_ += cfg_.priceImpact * imbalance_ + cfg_.midNoise * normal_(rng_);
        if (cfg_.midReversion > 0) {
            // The event has not been applied to the book yet, so this uses the book's
            // mid before it: a one-event lag that does not matter here.
            const auto bid = book.BestBid();
            const auto ask = book.BestAsk();
            if (bid && ask)
                mid_ += cfg_.midReversion * (0.5 * (*bid + *ask) - mid_);
        }
        return e;
    }

    double Mid() const { return mid_; }
    std::size_t Sweeps() const { return sweeps_; }
    std::uint64_t BadCancels() const { return badCancels_; }

private:
    // The next Hawkes arrival, or, with cancelRatePerOrder > 0, a per-order cancel if one
    // comes first. The two streams compete: per-order cancels form a Poisson process of
    // rate theta * (resting orders), which is constant between events, so its waiting time
    // can be redrawn after every event (memorylessness). The Hawkes arrival that lost the
    // race is kept for the next call.
    HawkesProcess::Arrival NextArrival(const ReferenceBook& book) {
        if (cfg_.cancelRatePerOrder <= 0)
            return hawkes_.Next();
        if (!pending_)
            pending_ = hawkes_.Next();
        const double rate = cfg_.cancelRatePerOrder * static_cast<double>(book.Size());
        if (rate > 0) {
            const double cancelTime = clock_ + std::exponential_distribution<double>{rate}(rng_);
            if (cancelTime < pending_->time) {
                clock_ = cancelTime;
                return {cancelTime, static_cast<std::size_t>(EventType::Cancel)};
            }
        }
        const auto arrival = *pending_;
        pending_.reset();
        clock_ = arrival.time;
        return arrival;
    }

    double DrawDistance() {
        double d = 0;
        if (cfg_.distanceLaw == DistanceLaw::Exponential) {
            d = std::exponential_distribution<double>{1.0 / cfg_.distanceMean}(rng_);
        } else {  // Lomax (Pareto II): P(D > x) = (1 + x/scale)^-shape
            const double u = 1.0 - uniform_(rng_);  // (0, 1]
            d = cfg_.powerLawScale * (std::pow(u, -1.0 / cfg_.powerLawShape) - 1.0);
        }
        return std::min(d, cfg_.maxDistance);
    }

    Quantity DrawSize() {
        const double x = std::lognormal_distribution<double>{cfg_.sizeLogMean, cfg_.sizeLogSigma}(rng_);
        return static_cast<Quantity>(std::clamp(std::round(x), 1.0, double(cfg_.maxSize)));
    }

    OrderId DrawCancelTarget(const ReferenceBook& book) {
        const auto live = book.RandomLiveId(rng_);
        if (live && uniform_(rng_) >= cfg_.badCancelProb)
            return *live;
        ++badCancels_;
        // Half: a past id that is no longer live (filled/cancelled); half: never existed.
        if (nextId_ > 1 && uniform_(rng_) < 0.5) {
            std::uniform_int_distribution<OrderId> past{1, nextId_ - 1};
            for (int tries = 0; tries < 8; ++tries)
                if (const OrderId id = past(rng_); !book.IsLive(id))
                    return id;
        }
        return kNeverExisted + nextId_;
    }

    static constexpr OrderId kNeverExisted = 1'000'000'000'000'000ull;

    FlowConfig cfg_;
    HawkesProcess hawkes_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    std::normal_distribution<double> normal_{0.0, 1.0};
    double mid_;
    double imbalance_ = 0.0;
    double lastTime_ = 0.0;
    double clock_ = 0.0;  // time of the last arrival, before timestamp rounding
    std::optional<HawkesProcess::Arrival> pending_;
    std::uint64_t seq_ = 0;
    OrderId nextId_ = 1;
    std::size_t sweeps_ = 0;
    std::uint64_t badCancels_ = 0;
};

}  // namespace sim
