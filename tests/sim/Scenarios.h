#pragma once

// Named order-flow regimes shared by the market-simulation tests and the live viewer.

#include <array>
#include <cmath>
#include <cstddef>

#include "Hawkes.h"
#include "OrderFlow.h"

namespace sim::scenarios {

// Indices: BuyLimit, SellLimit, BuyMarket, SellMarket, Cancel.
inline constexpr std::size_t BL = 0, SL = 1, BM = 2, SM = 3, CX = 4;
inline constexpr std::array<double, kEventTypes> kBaseMu{1.0, 1.0, 0.25, 0.25, 1.4};

inline FlowConfig WithMidDynamics(FlowConfig c) {
    c.priceImpact = 0.02;
    c.midNoise = 0.05;
    return c;
}

inline FlowConfig Poisson() {
    FlowConfig c;
    c.arrivals = UniformHawkes(kBaseMu, 0.0, 1.0);
    return WithMidDynamics(c);
}

inline FlowConfig Calm() {
    FlowConfig c;
    c.arrivals = UniformHawkes(kBaseMu, 0.2, 1.0);  // branching ratio 0.2
    return WithMidDynamics(c);
}

// Near-critical: strong self-excitation of every type, market orders trigger
// refills on the opposite side, fast decay => short violent bursts; bigger sizes.
inline FlowConfig Volatile() {
    FlowConfig c;
    const double beta = 5.0;
    HawkesParams p;
    p.mu.assign(kBaseMu.begin(), kBaseMu.end());
    p.beta = Constant(kEventTypes, beta);
    Matrix g = Constant(kEventTypes, 0.04);  // branching matrix alpha/beta
    for (std::size_t i = 0; i < kEventTypes; ++i)
        g[i][i] += 0.65;
    g[SL][BM] += 0.1;  // buy market orders excite sell-limit refills
    g[BL][SM] += 0.1;  // and vice versa
    p.alpha = g;
    for (auto& row : p.alpha)
        for (double& a : row)
            a *= beta;
    c.arrivals = p;
    c.sizeLogMean = 2.5;
    c.sizeLogSigma = 1.3;
    c = WithMidDynamics(c);
    c.priceImpact = 0.03;
    return c;
}

// One-sided crash: aggressive selling dominates, few buyers, selling is self-exciting.
inline FlowConfig Crash() {
    FlowConfig c;
    const double beta = 2.0;
    HawkesParams p;
    p.mu = {0.4, 1.4, 0.05, 1.0, 1.2};
    p.beta = Constant(kEventTypes, beta);
    Matrix g = Constant(kEventTypes, 0.03);
    g[SM][SM] += 0.5;
    g[SL][SM] += 0.15;
    p.alpha = g;
    for (auto& row : p.alpha)
        for (double& a : row)
            a *= beta;
    c.arrivals = p;
    c = WithMidDynamics(c);
    c.priceImpact = 0.03;
    return c;
}

// Many orders at the very same prices: mid pinned between two ticks, tiny distances.
inline FlowConfig Congested() {
    FlowConfig c;
    c.arrivals = UniformHawkes({2.0, 2.0, 0.3, 0.3, 0.4}, 0.2, 1.0);
    c.distanceMean = 0.3;
    c.sizeLogMean = 1.0;
    c.sizeLogSigma = 0.5;
    return c;  // no mid dynamics: price stays put
}

inline FlowConfig Simultaneous() {
    FlowConfig c = Calm();
    c.timestampResolution = 0.5;  // ~2 events per timestamp on average, more in bursts
    return c;
}

inline FlowConfig Sweep() {
    FlowConfig c = Calm();
    c.sweepProb = 0.05;
    c.sweepFactor = 1.5;
    c.sweepLevels = 5;
    return c;
}

inline FlowConfig PowerLawDistances() {
    FlowConfig c = Calm();
    c.distanceLaw = DistanceLaw::PowerLaw;
    return c;
}

inline FlowConfig Intraday(double dayLength) {
    FlowConfig c = Calm();
    // U-shape: s(x) = 0.5 + 6 (x - 1/2)^2 for x = time of day in [0, 1): 2 at open/close, 0.5 midday.
    c.arrivals.baselineShape = [dayLength](double t) {
        const double x = std::fmod(t, dayLength) / dayLength - 0.5;
        return 0.5 + 6.0 * x * x;
    };
    c.arrivals.baselineShapeMax = 2.0;
    return c;
}

// Turns any regime into one whose book stays deep, by adding three standard pieces
// of order-book modelling (the test scenarios above do not use it, so their numbers
// in docs/TESTING.md are unchanged):
//   * every resting order is cancelled at its own rate, so the cancel flow scales
//     with the book and the order count settles instead of wandering to zero
//     (the fixed-rate cancel stream is turned down to compensate);
//   * the generator's mid is pulled towards the book's mid, so orders are placed
//     around the visible quotes;
//   * a market order excites limit orders on the side it consumed (resiliency).
// Measured over 100k events (seed 1), share of events where a side has < 5 levels:
// Calm 39% -> 0.1%, Volatile 69% -> 0.1%.
inline FlowConfig StableBook(FlowConfig c) {
    c.cancelRatePerOrder = 0.01;
    c.arrivals.mu[CX] = 0.4;
    c.midReversion = 0.2;
    constexpr double refill = 0.15;  // extra children per market order (alpha / beta)
    c.arrivals.alpha[SL][BM] += refill * c.arrivals.beta[SL][BM];
    c.arrivals.alpha[BL][SM] += refill * c.arrivals.beta[BL][SM];
    return c;
}

}  // namespace sim::scenarios
