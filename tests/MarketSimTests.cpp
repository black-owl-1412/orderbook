// Market-realistic stochastic tests.
//
//   * Scenario.*      : engine vs reference model + invariant ledger, event by event,
//                       across parameter regimes (calm, volatile, crash, deep book, ...)
//   * Hawkes.*        : the arrival process itself (stationarity, rates)
//   * Realism.*       : statistical properties of the generated flow, so the
//                       scenario tests are known to exercise market-like input
//   * Calibration.*   : maximum-likelihood recovery of (mu, alpha, beta)
//
// All randomness is seeded; every run is reproducible. Every statistic is printed
// so the numbers in docs come from actual runs.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <print>
#include <string>
#include <vector>

#include "sim/Driver.h"
#include "sim/Hawkes.h"
#include "sim/OrderFlow.h"
#include "sim/Stats.h"

using namespace sim;

namespace {

// Indices: BuyLimit, SellLimit, BuyMarket, SellMarket, Cancel.
constexpr std::size_t BL = 0, SL = 1, BM = 2, SM = 3, CX = 4;
constexpr std::array<double, kEventTypes> kBaseMu{1.0, 1.0, 0.25, 0.25, 1.4};

FlowConfig WithMidDynamics(FlowConfig c) {
    c.priceImpact = 0.02;
    c.midNoise = 0.05;
    return c;
}

FlowConfig Poisson() {
    FlowConfig c;
    c.arrivals = UniformHawkes(kBaseMu, 0.0, 1.0);
    return WithMidDynamics(c);
}

FlowConfig Calm() {
    FlowConfig c;
    c.arrivals = UniformHawkes(kBaseMu, 0.2, 1.0);  // branching ratio 0.2
    return WithMidDynamics(c);
}

// Near-critical: strong self-excitation of every type, market orders trigger
// refills on the opposite side, fast decay => short violent bursts; bigger sizes.
FlowConfig Volatile() {
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
FlowConfig Crash() {
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
FlowConfig Congested() {
    FlowConfig c;
    c.arrivals = UniformHawkes({2.0, 2.0, 0.3, 0.3, 0.4}, 0.2, 1.0);
    c.distanceMean = 0.3;
    c.sizeLogMean = 1.0;
    c.sizeLogSigma = 0.5;
    return c;  // no mid dynamics: price stays put
}

FlowConfig Simultaneous() {
    FlowConfig c = Calm();
    c.timestampResolution = 0.5;  // ~2 events per timestamp on average, more in bursts
    return c;
}

FlowConfig Sweep() {
    FlowConfig c = Calm();
    c.sweepProb = 0.05;
    c.sweepFactor = 1.5;
    c.sweepLevels = 5;
    return c;
}

FlowConfig PowerLawDistances() {
    FlowConfig c = Calm();
    c.distanceLaw = DistanceLaw::PowerLaw;
    return c;
}

FlowConfig Intraday(double dayLength) {
    FlowConfig c = Calm();
    // U-shape: s(x) = 0.5 + 6 (x - 1/2)^2 for x = time of day in [0, 1): 2 at open/close, 0.5 midday.
    c.arrivals.baselineShape = [dayLength](double t) {
        const double x = std::fmod(t, dayLength) / dayLength - 0.5;
        return 0.5 + 6.0 * x * x;
    };
    c.arrivals.baselineShapeMax = 2.0;
    return c;
}

void PrintStats(const std::string& name, const RunResult& r) {
    const RunStats& s = r.stats;
    std::println("[{}] events={} trades={} rejects={} resting_end={} max_levels_swept={} "
                 "longest_queue={} tied_ts={} sweeps={} bad_cancels={} mid {:.1f} -> {:.1f}",
                 name, r.stream.size(), s.trades, s.rejects, s.restingAtEnd, s.maxLevelsSwept,
                 s.longestQueue, s.tiedTimestamps, s.sweeps, s.badCancels, s.startMid, s.endMid);
}

RunResult RunChecked(const std::string& name, const FlowConfig& cfg, std::uint64_t seed,
                     RunOptions opt = {}, const Prefill& prefill = {}) {
    opt.name = name + "_seed" + std::to_string(seed);
    RunResult r = RunScenario(cfg, seed, opt, prefill);
    EXPECT_FALSE(r.failure.has_value()) << FormatFailure(r);
    PrintStats(opt.name, r);
    return r;
}

}  // namespace

// ============================ Scenarios =====================================

TEST(Scenario, PoissonBaseline) {
    for (std::uint64_t seed : {1, 2, 3})
        RunChecked("poisson", Poisson(), seed);
}

TEST(Scenario, Calm) {
    for (std::uint64_t seed : {1, 2, 3})
        RunChecked("calm", Calm(), seed);
}

TEST(Scenario, VolatileBurst) {
    for (std::uint64_t seed : {1, 2, 3})
        RunChecked("volatile", Volatile(), seed);
}

TEST(Scenario, OneSidedCrash) {
    for (std::uint64_t seed : {1, 2, 3}) {
        const auto r = RunChecked("crash", Crash(), seed);
        EXPECT_LT(r.stats.endMid, r.stats.startMid) << "a sell-dominated flow should push the mid down";
    }
}

TEST(Scenario, EmptyStart) {
    // Market orders and cancels against an empty book must be rejected identically
    // by engine and reference from the very first event.
    FlowConfig c = Calm();
    c.arrivals.mu = {0.2, 0.2, 1.0, 1.0, 1.0};  // mostly markets/cancels at first
    RunOptions opt;
    opt.events = 2'000;
    const auto r = RunChecked("empty_start", c, 11, opt);
    EXPECT_GT(r.stats.rejects, 0u);
}

TEST(Scenario, DeepBook) {
    // One million resting orders 200..5199 ticks away from the mid, then live flow.
    constexpr int kSeedOrders = 1'000'000;
    const Prefill prefill = [](Seeder& s) -> std::optional<std::string> {
        for (int i = 0; i < kSeedOrders; ++i) {
            const Price offset = 200 + (i / 2) % 5'000;
            const bool buy = i % 2 == 0;
            if (auto err = s.Add(buy ? Side::Buy : Side::Sell, buy ? 10'000 - offset : 10'001 + offset,
                                 static_cast<Quantity>(1 + i % 50)))
                return err;
        }
        return std::nullopt;
    };
    RunOptions opt;
    opt.events = 20'000;
    opt.levelCheckEvery = 500;
    opt.levelTotalsEvery = 5'000;
    opt.depthCompareEvery = 5'000;
    opt.depthSampleEvery = 0;
    const auto r = RunChecked("deep_book", Calm(), 21, opt, prefill);
    EXPECT_GE(r.stats.restingAtEnd, 900'000u);
}

TEST(Scenario, CongestedLevel) {
    const auto r = RunChecked("congested", Congested(), 31);
    EXPECT_GT(r.stats.longestQueue, 100u);
}

TEST(Scenario, SimultaneousEvents) {
    const auto r = RunChecked("simultaneous", Simultaneous(), 41);
    EXPECT_GT(r.stats.tiedTimestamps, 1'000u);
}

TEST(Scenario, Sweep) {
    const auto r = RunChecked("sweep", Sweep(), 51);
    EXPECT_GT(r.stats.sweeps, 100u);
    EXPECT_GE(r.stats.maxLevelsSwept, 5u);
}

TEST(Scenario, PowerLawDistances) {
    RunChecked("powerlaw", PowerLawDistances(), 61);
}

// ============================ Hawkes process ================================

TEST(Hawkes, BranchingRatios) {
    const double calm = BranchingRatio(Calm().arrivals);
    const double vol = BranchingRatio(Volatile().arrivals);
    const double crash = BranchingRatio(Crash().arrivals);
    std::println("branching ratio: poisson={:.4f} calm={:.4f} volatile={:.4f} crash={:.4f}",
                 BranchingRatio(Poisson().arrivals), calm, vol, crash);
    EXPECT_NEAR(calm, 0.2, 1e-6);
    EXPECT_GT(vol, 0.85);
    EXPECT_LT(vol, 1.0);
    EXPECT_LT(crash, 1.0);
}

TEST(Hawkes, RejectsNonStationaryParameters) {
    EXPECT_THROW((HawkesProcess{UniformHawkes(kBaseMu, 1.0, 1.0), 1}), std::invalid_argument);
    EXPECT_THROW((HawkesProcess{UniformHawkes(kBaseMu, 1.3, 1.0), 1}), std::invalid_argument);
}

TEST(Hawkes, MeanRateMatchesTheory) {
    // Stationary univariate rate = mu / (1 - n).
    for (double n : {0.0, 0.5, 0.8}) {
        HawkesParams p;
        p.mu = {1.0};
        p.alpha = {{n * 2.0}};
        p.beta = {{2.0}};
        HawkesProcess h{p, 7};
        const std::size_t count = 200'000;
        double t = 0;
        for (std::size_t i = 0; i < count; ++i)
            t = h.Next().time;
        const double rate = static_cast<double>(count) / t;
        std::println("n={:.1f}: measured rate {:.4f}, theory {:.4f}", n, rate, 1.0 / (1.0 - n));
        EXPECT_NEAR(rate, 1.0 / (1.0 - n), 0.05 / (1.0 - n));
    }
}

// ============================ Realism =======================================

namespace {
RunOptions StatsRun(std::size_t events) {
    RunOptions o;
    o.events = events;
    o.levelTotalsEvery = 1'000;
    o.depthCompareEvery = 1'000;
    return o;
}

// Statistics runs also drive the engine, so they must be failure-free too.
RunResult RunQuiet(const FlowConfig& cfg, std::uint64_t seed, RunOptions opt) {
    opt.name = "stats_seed" + std::to_string(seed);
    RunResult r = RunScenario(cfg, seed, opt);
    EXPECT_FALSE(r.failure.has_value()) << FormatFailure(r);
    return r;
}
}  // namespace

TEST(Realism, ArrivalClustering) {
    const auto poisson = RunQuiet(Poisson(), 101, StatsRun(50'000));
    const auto calm = RunQuiet(Calm(), 101, StatsRun(50'000));
    const auto vol = RunQuiet(Volatile(), 101, StatsRun(50'000));
    for (double w : {1.0, 10.0}) {
        const double vp = stats::VarianceToMean(stats::CountsPerWindow(poisson.stats.times, w));
        const double vc = stats::VarianceToMean(stats::CountsPerWindow(calm.stats.times, w));
        const double vv = stats::VarianceToMean(stats::CountsPerWindow(vol.stats.times, w));
        std::println("variance/mean of counts, window {}: poisson={:.3f} calm={:.3f} volatile={:.3f}",
                     w, vp, vc, vv);
        EXPECT_NEAR(vp, 1.0, 0.1);
        EXPECT_GT(vc, vp);
        EXPECT_GT(vv, 2.0 * vp);
    }
}

TEST(Realism, TradeSignAutocorrelation) {
    const auto poisson = RunQuiet(Poisson(), 102, StatsRun(100'000));
    const auto vol = RunQuiet(Volatile(), 102, StatsRun(100'000));
    std::print("trade-sign ACF lag:      ");
    for (std::size_t lag : {1, 2, 5, 10, 20, 50}) std::print("{:>8}", lag);
    std::print("\n  poisson               ");
    for (std::size_t lag : {1, 2, 5, 10, 20, 50})
        std::print("{:>8.4f}", stats::Autocorrelation(poisson.stats.aggressorSigns, lag));
    std::print("\n  volatile              ");
    for (std::size_t lag : {1, 2, 5, 10, 20, 50})
        std::print("{:>8.4f}", stats::Autocorrelation(vol.stats.aggressorSigns, lag));
    std::println("\n  ({} / {} signed aggressive events)", poisson.stats.aggressorSigns.size(),
                 vol.stats.aggressorSigns.size());
    const auto& s = vol.stats.aggressorSigns;
    EXPECT_GT(stats::Autocorrelation(s, 1), 0.0);
    EXPECT_GT(stats::Autocorrelation(s, 10), 0.0);
    EXPECT_GT(stats::Autocorrelation(s, 1), stats::Autocorrelation(s, 10));
}

TEST(Realism, HeavyTailedSizes) {
    const auto vol = RunQuiet(Volatile(), 103, StatsRun(50'000));
    const auto& sizes = vol.stats.sizes;
    const double kurt = stats::Kurtosis(sizes);
    const double hill = stats::HillTailIndex(sizes, sizes.size() / 100);
    std::vector<double> sorted = sizes;
    std::ranges::sort(sorted);
    std::println("sizes: n={} median={} p99={} max={} kurtosis={:.1f} hill(top1%)={:.2f}",
                 sizes.size(), sorted[sorted.size() / 2], sorted[sorted.size() * 99 / 100],
                 sorted.back(), kurt, hill);
    EXPECT_GT(kurt, 3.0);
}

TEST(Realism, DepthDecreasesWithDistance) {
    const auto calm = RunQuiet(Calm(), 104, StatsRun(50'000));
    const auto& d = calm.stats.depthProfile;
    std::print("mean resting qty per side by distance from mid (ticks):\n ");
    for (std::size_t i = 0; i < d.size(); ++i)
        std::print(" {}:{:.0f}", i, d[i]);
    std::println("");
    auto band = [&](std::size_t from, std::size_t to) {
        double s = 0;
        for (std::size_t i = from; i < to; ++i) s += d[i];
        return s / static_cast<double>(to - from);
    };
    const double b1 = band(2, 6), b2 = band(6, 12), b3 = band(12, 24), b4 = band(24, 40);
    std::println("bands [2,6)={:.1f} [6,12)={:.1f} [12,24)={:.1f} [24,40)={:.1f}", b1, b2, b3, b4);
    EXPECT_GT(b1, b2);
    EXPECT_GT(b2, b3);
    EXPECT_GT(b3, b4);
}

TEST(Realism, FatTailedMidReturns) {
    for (auto [name, cfg] : {std::pair{"poisson", Poisson()}, std::pair{"volatile", Volatile()}}) {
        const auto r = RunQuiet(cfg, 105, StatsRun(100'000));
        const auto returns = stats::Differences(r.stats.mids);
        std::println("{}: {} mid returns, kurtosis {:.2f}", name, returns.size(),
                     stats::Kurtosis(returns));
    }
    const auto vol = RunQuiet(Volatile(), 105, StatsRun(100'000));
    EXPECT_GT(stats::Kurtosis(stats::Differences(vol.stats.mids)), 3.0);
}

TEST(Realism, IntradayUShape) {
    const double day = 10'000.0;
    const auto r = RunQuiet(Intraday(day), 106, StatsRun(60'000));
    std::array<double, 10> buckets{};
    for (double t : r.stats.times)
        if (t < day)
            buckets[static_cast<std::size_t>(t / day * 10)] += 1;
    std::print("events per tenth of the day:");
    for (double b : buckets) std::print(" {:.0f}", b);
    std::println("");
    EXPECT_GT(buckets.front(), 2.0 * buckets[5]);
    EXPECT_GT(buckets.back(), 2.0 * buckets[5]);
}

// ============================ Calibration ===================================

TEST(Calibration, RecoversHawkesParametersByMaximumLikelihood) {
    struct Truth { double mu, alpha, beta; };
    for (Truth truth : {Truth{1.0, 0.3, 1.5}, Truth{0.5, 1.8, 2.0}}) {  // n = 0.2 and 0.9
        HawkesParams p;
        p.mu = {truth.mu};
        p.alpha = {{truth.alpha}};
        p.beta = {{truth.beta}};
        HawkesProcess h{p, 2024};
        std::vector<double> times;
        for (int i = 0; i < 50'000; ++i)
            times.push_back(h.Next().time);
        const double T = times.back();

        auto negLL = [&](const std::vector<double>& x) {
            return -stats::HawkesLogLikelihood(times, T, std::exp(x[0]), std::exp(x[1]), std::exp(x[2]));
        };
        const auto fit = stats::NelderMead(negLL, {0.0, -1.0, 0.0});
        const double mu = std::exp(fit[0]), alpha = std::exp(fit[1]), beta = std::exp(fit[2]);
        std::println("truth mu={:.3f} alpha={:.3f} beta={:.3f} n={:.3f}  ->  fit mu={:.3f} alpha={:.3f} "
                     "beta={:.3f} n={:.3f}",
                     truth.mu, truth.alpha, truth.beta, truth.alpha / truth.beta, mu, alpha, beta,
                     alpha / beta);
        EXPECT_NEAR(alpha / beta, truth.alpha / truth.beta, 0.05);
        EXPECT_NEAR(mu, truth.mu, 0.15 * truth.mu);
        EXPECT_NEAR(beta, truth.beta, 0.2 * truth.beta);
    }
}
