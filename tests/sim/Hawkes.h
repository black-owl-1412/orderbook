#pragma once

// Multivariate Hawkes process with exponential kernels, simulated by Ogata thinning.
//
//   lambda_i(t) = mu_i * s(t) + sum_j sum_{t_k of type j, t_k < t} alpha_ij * exp(-beta_ij (t - t_k))
//
// s(t) is an optional baseline shape (e.g. intraday U-curve); s == 1 if not given.
// A homogeneous Poisson process is the special case alpha == 0.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sim {

using Matrix = std::vector<std::vector<double>>;

struct HawkesParams {
    std::vector<double> mu;  // baseline intensity per type
    Matrix alpha;            // alpha[i][j]: jump in lambda_i caused by an event of type j
    Matrix beta;             // beta[i][j]: decay speed of that jump
    std::function<double(double)> baselineShape;  // optional s(t) >= 0
    double baselineShapeMax = 1.0;                 // sup s(t); thinning needs an upper bound

    std::size_t Dim() const { return mu.size(); }
};

inline Matrix Constant(std::size_t n, double value) {
    return Matrix(n, std::vector<double>(n, value));
}

// Spectral radius of the branching matrix G_ij = alpha_ij / beta_ij: the expected
// number of direct children per event. Stationarity requires it to be < 1.
// G is non-negative, so its Perron root is found by power iteration on G + I
// (the shift avoids oscillation for periodic matrices).
inline double BranchingRatio(const HawkesParams& p) {
    const std::size_t n = p.Dim();
    std::vector<double> v(n, 1.0), w(n);
    double estimate = 0.0;
    for (int it = 0; it < 100'000; ++it) {
        double norm = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            w[i] = v[i];
            for (std::size_t j = 0; j < n; ++j)
                if (p.alpha[i][j] != 0.0)
                    w[i] += p.alpha[i][j] / p.beta[i][j] * v[j];
            norm = std::max(norm, w[i]);
        }
        for (std::size_t i = 0; i < n; ++i)
            v[i] = w[i] / norm;
        if (std::abs(norm - estimate) < 1e-13)
            return norm - 1.0;
        estimate = norm;
    }
    return estimate - 1.0;
}

class HawkesProcess {
public:
    struct Arrival {
        double time;
        std::size_t type;
    };

    HawkesProcess(HawkesParams params, std::uint64_t seed)
        : p_{std::move(params)},
          rng_{seed},
          excite_(p_.Dim(), std::vector<double>(p_.Dim(), 0.0)) {
        const std::size_t n = p_.Dim();
        if (n == 0 || p_.alpha.size() != n || p_.beta.size() != n)
            throw std::invalid_argument("Hawkes: inconsistent dimensions");
        for (std::size_t i = 0; i < n; ++i) {
            if (p_.alpha[i].size() != n || p_.beta[i].size() != n || p_.mu[i] < 0)
                throw std::invalid_argument("Hawkes: bad row");
            for (std::size_t j = 0; j < n; ++j)
                if (p_.alpha[i][j] < 0 || (p_.alpha[i][j] > 0 && p_.beta[i][j] <= 0))
                    throw std::invalid_argument("Hawkes: need alpha >= 0 and beta > 0");
        }
        // Power iteration converges to the root from below, so treat "within 1e-9 of 1" as critical.
        if (BranchingRatio(p_) >= 1.0 - 1e-9)
            throw std::invalid_argument("Hawkes: branching ratio >= 1 (non-stationary)");
    }

    // Ogata thinning. Between events every excitation term only decays, and the
    // baseline is bounded by mu * baselineShapeMax, so the intensity at the current
    // time with the baseline at its maximum bounds the intensity until the next
    // accepted event. Propose with that rate and accept with probability lambda/upper.
    Arrival Next() {
        for (;;) {
            double upper = 0.0;
            for (std::size_t i = 0; i < p_.Dim(); ++i) {
                upper += p_.mu[i] * p_.baselineShapeMax;
                for (double e : excite_[i])
                    upper += e;
            }
            if (upper <= 0.0)
                throw std::logic_error("Hawkes: zero intensity, no further events");

            Advance(std::exponential_distribution<double>{upper}(rng_));

            const double u = std::uniform_real_distribution<double>{0.0, upper}(rng_);
            double cumulative = 0.0;
            for (std::size_t i = 0; i < p_.Dim(); ++i) {
                cumulative += Intensity(i);
                if (u < cumulative) {
                    for (std::size_t k = 0; k < p_.Dim(); ++k)
                        excite_[k][i] += p_.alpha[k][i];
                    return {now_, i};
                }
            }
            // rejected candidate: continue from the candidate time
        }
    }

    double Intensity(std::size_t i) const {
        const double s = p_.baselineShape ? p_.baselineShape(now_) : 1.0;
        double lambda = p_.mu[i] * s;
        for (double e : excite_[i])
            lambda += e;
        return lambda;
    }

    double Now() const { return now_; }
    const HawkesParams& Params() const { return p_; }

private:
    void Advance(double dt) {
        now_ += dt;
        for (std::size_t i = 0; i < p_.Dim(); ++i)
            for (std::size_t j = 0; j < p_.Dim(); ++j)
                if (excite_[i][j] != 0.0)
                    excite_[i][j] *= std::exp(-p_.beta[i][j] * dt);
    }

    HawkesParams p_;
    std::mt19937_64 rng_;
    Matrix excite_;  // excite_[i][j]: current contribution of past type-j events to lambda_i
    double now_ = 0.0;
};

}  // namespace sim
