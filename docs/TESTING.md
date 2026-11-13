# Testing

Three layers, all run by `ctest` (81 tests; about 74 s under ASan+UBSan with `-j4`, most of it the 1M-order deep-book scenario).

| Suite | File | What it does |
|---|---|---|
| Unit | `tests/OrderBookTests.cpp` | 41 hand-written cases, one behaviour each |
| Differential | `tests/DifferentialTests.cpp` | 20 seeds × 5,000 random ops covering every order type, modify and end-of-day, checked step by step against the reference book |
| Market simulation | `tests/MarketSimTests.cpp` | Stochastic, market-like order flow in several regimes, checked by a reference model **and** an invariant ledger after every event, plus statistical checks that the flow is realistic |

Every random source is seeded, so every run is reproducible.

## Market simulation

### Order-flow model (`tests/sim/`)
- **Arrivals (`Hawkes.h`)**: a multivariate Hawkes process, one dimension per event type (buy limit, sell limit, buy market, sell market, cancel), simulated by Ogata thinning. `alpha_ij` is how much a type-j event excites type i. Poisson is the case alpha = 0. Construction rejects any configuration where the spectral radius of `alpha/beta` is ≥ 1. An optional baseline shape `mu(t)` gives an intraday U-curve.
- **Attributes (`OrderFlow.h`)**:
  - Limit price is mid ∓ d, rounded outward to the tick grid, with d drawn from an exponential or Lomax (power-law) distribution.
  - Size is log-normal, rounded, and at least 1.
  - A cancel picks uniformly among live orders, with a 2% chance of targeting a dead or never-existing id.
  - The mid moves with decayed order-flow imbalance plus Gaussian noise.
  - Sweep orders are sized to 1.5× the visible depth of the top 5 levels.
- **Oracle (`ReferenceBook.h`)**: one `std::map` per side keyed by (price priority, arrival sequence), so `begin()` is always the order that trades next.
- **Ledger (`Ledger.h`)**: rebuilds the live order set from the *engine's own* outputs and checks the following after every event:
  - the book is not crossed;
  - each trade hits the best price level, and the earliest order within that level;
  - trade price equals the resting order's price;
  - no overfill, and no zero-quantity trades or levels;
  - every reject is justified;
  - conservation: `added = resting + 2·traded + cancelled`.
- **Driver (`Driver.h`)** stops at the first discrepancy. It reports the shortest failing prefix (failing index + 1 events), prints the last events and writes the prefix to `failing_prefix_<scenario>_seed<N>.txt`.

### Scenarios (20,000 events each unless noted; measured)

| Scenario | Seeds | Trades | Rejects | Notes |
|---|---|---|---|---|
| Poisson baseline | 1–3 | 5,347–5,393 | 2–22 | |
| Calm (n = 0.2) | 1–3 | 6,704–6,863 | 66–98 | |
| Volatile burst (n = 0.884) | 1–3 | 9,637–9,744 (seeds 2–3) | 926–1,120 | up to 16 levels swept by one order |
| One-sided crash (n = 0.549) | 1–3 | 4,966–5,158 | 4,392–4,493 | mid 10,000.5 → about 6,300 |
| Empty start (2,000 events) | 11 | 244 | 880 | market orders and cancels on an empty book |
| Deep book (1,000,000 seed orders) | 21 | 9,122 | 0 | 997,442 orders resting at the end |
| Congested level | 31 | 5,762 | 0 | 3,437 orders in one FIFO queue |
| Simultaneous events | 41 | 6,324 | 203 | 12,597 events share a timestamp |
| Sweep | 51 | 6,183 | 368 | 173 sweep orders, up to 10 levels each |
| Power-law distances | 61 | 6,367 | 234 | |

The engine produced **no mismatch and no invariant violation** in any scenario.

### Is the flow realistic? (measured)

| Property | Result |
|---|---|
| Hawkes mean rate vs theory `mu/(1-n)` | n = 0: 1.0021 vs 1.0000; n = 0.5: 2.0193 vs 2.0000; n = 0.8: 5.0938 vs 5.0000 |
| Clustering: variance/mean of counts (window 1 / window 10) | Poisson 0.993 / 1.044; calm 1.163 / 1.573; volatile 18.604 / 57.372 |
| Trade-sign ACF, volatile, lags 1, 2, 5, 10, 20, 50 | 0.328, 0.292, 0.218, 0.161, 0.102, 0.040 (Poisson: 0.026–0.049) |
| Sizes (volatile) | median 12, p99 250, max 3,461, kurtosis 472.9, Hill index (top 1%) 2.22 |
| Mean depth per side by distance band (calm) | [2,6) ticks 26.4; [6,12) 7.6; [12,24) 0.6; [24,40) 0.0 |
| Mid-return kurtosis | Poisson 24.47 (25,540 returns); volatile 43.34 (3,039 returns) |
| Intraday U-shape, events per tenth of the day | 8,355, 5,947, 4,326, 3,052, 2,492, 2,561, 3,117, 4,272, 6,172, 8,257 |

Caveats, read from the same numbers:
- **The sign autocorrelation decays roughly geometrically, not like a power law.** Exponential Hawkes kernels have short memory. Real markets show long memory, which would need power-law kernels or meta-orders.
- **Fat-tailed mid returns appear in the Poisson baseline too.** They come mostly from book mechanics (the mid jumps when the touch level empties), not from Hawkes clustering.
- **The calm book is thin:** it ends with 2–37 resting orders. Depth peaks 2 ticks from the mid and then decreases.
- **The crash regime is a stress test, not a calibrated one.** The mid falls about 37% in 20,000 events.
- **The market simulation has only the five event types from the spec.** FAK, FOK, GFD and modify are covered by the differential suite.
- **Simultaneous events:** the engine has no clock, so events that share a timestamp are processed in stream order.

### Calibration
Maximum-likelihood fit (Nelder–Mead on the exact exponential-Hawkes log-likelihood) on 50,000 simulated events:

| Truth (mu, alpha, beta, n) | Fit |
|---|---|
| 1.000, 0.300, 1.500, 0.200 | 0.988, 0.284, 1.358, 0.209 |
| 0.500, 1.800, 2.000, 0.900 | 0.512, 1.839, 2.053, 0.896 |

This shows the estimator recovers known parameters. Calibrating on a real order-book sample needs that sample: fit with `stats::HawkesLogLikelihood`, then run the realism checks above on the fitted flow.

## Mutation check
Each bug was planted one at a time; a suite catches it if any of its tests fails.

| Planted bug | Unit | Differential | Market sim |
|---|---|---|---|
| FOK exact-quantity check `>=` changed to `>` | caught | caught | missed (no FOK in this flow) |
| Trade at aggressor price | caught | caught | caught |
| Bids LIFO instead of FIFO | caught | caught | caught |
| Level total not reduced on cancel | caught | caught | caught |
| FAK/market sell remainder kept | caught | caught | caught |

Before boundary-sized FOK orders were added to the differential generator, it missed the first bug.

## Running
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DORDERBOOK_SANITIZE=ON
cmake --build build -j
ctest --test-dir build -j4 --output-on-failure
./build/orderbook_tests --gtest_filter='Realism.*:Calibration.*'   # prints all the statistics above
```
