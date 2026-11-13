#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <numeric>
#include <vector>

namespace sim::stats {

inline double Mean(const std::vector<double>& x) {
    return x.empty() ? 0.0 : std::accumulate(x.begin(), x.end(), 0.0) / static_cast<double>(x.size());
}

inline double CentralMoment(const std::vector<double>& x, int k) {
    const double m = Mean(x);
    double s = 0.0;
    for (double v : x)
        s += std::pow(v - m, k);
    return s / static_cast<double>(x.size());
}

inline double Variance(const std::vector<double>& x) { return CentralMoment(x, 2); }

// Plain (non-excess) kurtosis: 3 for a Gaussian, larger = fatter tails.
inline double Kurtosis(const std::vector<double>& x) {
    const double v = Variance(x);
    return v > 0 ? CentralMoment(x, 4) / (v * v) : 0.0;
}

inline double Autocorrelation(const std::vector<double>& x, std::size_t lag) {
    if (x.size() <= lag + 1)
        return 0.0;
    const double m = Mean(x);
    double num = 0.0, den = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        den += (x[i] - m) * (x[i] - m);
        if (i + lag < x.size())
            num += (x[i] - m) * (x[i + lag] - m);
    }
    return den > 0 ? num / den : 0.0;
}

// Number of events in consecutive windows of width `window`.
inline std::vector<double> CountsPerWindow(const std::vector<double>& times, double window) {
    std::vector<double> counts;
    if (times.empty())
        return counts;
    counts.assign(static_cast<std::size_t>(times.back() / window) + 1, 0.0);
    for (double t : times)
        counts[static_cast<std::size_t>(t / window)] += 1.0;
    counts.pop_back();  // last window is incomplete
    return counts;
}

// Index of dispersion: 1 for Poisson counts, > 1 for clustered arrivals.
inline double VarianceToMean(const std::vector<double>& counts) {
    const double m = Mean(counts);
    return m > 0 ? Variance(counts) / m : 0.0;
}

// Hill estimator of the tail exponent from the k largest observations.
// Smaller = heavier tail (a Gaussian has none; power laws in finance are ~2-4).
inline double HillTailIndex(std::vector<double> x, std::size_t k) {
    std::ranges::sort(x, std::greater<>{});
    k = std::min(k, x.size() - 1);
    double s = 0.0;
    for (std::size_t i = 0; i < k; ++i)
        s += std::log(x[i] / x[k]);
    return s > 0 ? static_cast<double>(k) / s : 0.0;
}

inline std::vector<double> Differences(const std::vector<double>& x) {
    std::vector<double> d;
    for (std::size_t i = 1; i < x.size(); ++i)
        d.push_back(x[i] - x[i - 1]);
    return d;
}

// Minimal Nelder-Mead minimiser (enough for 3-parameter likelihood fits).
inline std::vector<double> NelderMead(const std::function<double(const std::vector<double>&)>& f,
                                      std::vector<double> start, double step = 0.5,
                                      int iterations = 2'000) {
    const std::size_t n = start.size();
    std::vector<std::vector<double>> simplex{start};
    for (std::size_t i = 0; i < n; ++i) {
        auto p = start;
        p[i] += step;
        simplex.push_back(p);
    }
    std::vector<double> values;
    for (const auto& p : simplex)
        values.push_back(f(p));

    for (int it = 0; it < iterations; ++it) {
        std::vector<std::size_t> order(n + 1);
        std::iota(order.begin(), order.end(), 0);
        std::ranges::sort(order, [&](auto a, auto b) { return values[a] < values[b]; });
        const std::size_t best = order.front(), worst = order.back(), second = order[n - 1];
        if (std::abs(values[worst] - values[best]) < 1e-10)
            break;

        std::vector<double> centroid(n, 0.0);
        for (std::size_t i : order)
            if (i != worst)
                for (std::size_t d = 0; d < n; ++d)
                    centroid[d] += simplex[i][d] / static_cast<double>(n);
        auto along = [&](double t) {
            std::vector<double> p(n);
            for (std::size_t d = 0; d < n; ++d)
                p[d] = centroid[d] + t * (simplex[worst][d] - centroid[d]);
            return p;
        };

        const auto reflected = along(-1.0);
        const double fr = f(reflected);
        if (fr < values[best]) {
            const auto expanded = along(-2.0);
            const double fe = f(expanded);
            simplex[worst] = fe < fr ? expanded : reflected;
            values[worst] = std::min(fe, fr);
        } else if (fr < values[second]) {
            simplex[worst] = reflected;
            values[worst] = fr;
        } else {
            const auto contracted = along(0.5);
            const double fc = f(contracted);
            if (fc < values[worst]) {
                simplex[worst] = contracted;
                values[worst] = fc;
            } else {  // shrink towards best
                for (std::size_t i = 0; i <= n; ++i)
                    if (i != best) {
                        for (std::size_t d = 0; d < n; ++d)
                            simplex[i][d] = simplex[best][d] + 0.5 * (simplex[i][d] - simplex[best][d]);
                        values[i] = f(simplex[i]);
                    }
            }
        }
    }
    const auto it = std::ranges::min_element(values);
    return simplex[static_cast<std::size_t>(it - values.begin())];
}

// Exact log-likelihood of a univariate exponential Hawkes process on [0, T].
inline double HawkesLogLikelihood(const std::vector<double>& t, double T, double mu, double alpha,
                                  double beta) {
    double ll = 0.0, A = 0.0;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (i > 0)
            A = std::exp(-beta * (t[i] - t[i - 1])) * (1.0 + A);
        ll += std::log(mu + alpha * A);
    }
    ll -= mu * T;
    for (double ti : t)
        ll -= alpha / beta * (1.0 - std::exp(-beta * (T - ti)));
    return ll;
}

}  // namespace sim::stats
