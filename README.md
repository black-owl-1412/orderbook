# Orderbook

A limit order book and matching engine written in C++23. It is the first piece of a larger exchange simulator.

## Features
- **Price-time priority** matching: the best price fills first, and orders at the same price fill in arrival order (FIFO).
- Order types:
  - **GoodTillCancel**: the order rests in the book until it is filled or cancelled.
  - **GoodForDay**: the order rests like GTC and expires at `CancelGoodForDayOrders()` (end of day).
  - **FillAndKill** (IOC): the order fills what it can right away and the rest is cancelled.
  - **FillOrKill**: the order fills its whole quantity right away, or it is rejected and the book is untouched.
  - **Market**: the order takes liquidity at any price and the rest is cancelled. It is rejected if the other side is empty.
- Add, cancel and modify (cancel/replace; a modified order loses its time priority).
- Rejected requests come back with a reason, using `std::expected<Trades, RejectReason>`: `InvalidOrder`, `ZeroQuantity`, `DuplicateOrderId`, `UnknownOrderId`, `CannotMatch`, `CannotFullyFill`.
- Trades execute at the **resting (maker) order's price**.
- Depth snapshots aggregated per price level.

## Design

| Structure | Purpose | Cost |
|---|---|---|
| `std::flat_map<Price, Level>` per side, sorted so the best price is last | price levels | O(log L) to find a level, O(1) to reach or remove the best level |
| `Level`: head and tail of an intrusive FIFO queue, plus the level's total quantity | orders at one price | O(1) to append, unlink or read the total |
| `ObjectPool<OrderNode>` | storage for resting orders | no heap allocation per order |
| `std::unordered_map<OrderId, OrderNode*>` | look up an order by id | O(1) average cancel |

An incoming order trades against the other side first, and only what is left is placed in the book. Each resting order is a pooled node that holds the links to its queue neighbours, so it can be unlinked without a search.

`-DORDERBOOK_FLAT_LEVELS=OFF` builds with `std::map` for the price levels instead.

## Performance
Measured against the original `shared_ptr` + `std::list` version on the same machine:

| Benchmark | Original | Current |
|---|---|---|
| Mixed workload, 1M operations | 185 ms (5.4M ops/s) | 82.2 ms (12.2M ops/s) |
| Limit add latency, p50 / p99 | 166 / 1,131 ns | 93 / 331 ns |
| Depth snapshot, 20k orders | 185 µs | 0.147 µs |

See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) for each optimisation step, the method and the limits of these numbers.

## Layout
```
include/orderbook/   public headers (Types, Order, OrderModify, Trade, LevelInfo, ObjectPool, OrderBook)
src/                 matching engine implementation
apps/main.cpp        demo
tests/               GoogleTest suite; tests/sim/ holds the market simulation
bench/               benchmarks, latency tool and recorded results
docs/                TESTING.md, PERFORMANCE.md
legacy/main_v0.cpp   original single-file version (not built)
```

## Build and test
Requires CMake ≥ 3.28 and a C++23 compiler (tested with GCC 15). GoogleTest comes from the system if installed; otherwise CMake downloads it.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DORDERBOOK_SANITIZE=ON   # ASan + UBSan
cmake --build build -j
ctest --test-dir build -j4 --output-on-failure
./build/orderbook_demo

# benchmarks (Release)
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DORDERBOOK_BUILD_TESTS=OFF
cmake --build build-release -j
./build-release/orderbook_bench
./build-release/orderbook_latency
```

## Testing
There are 81 tests in three layers: unit tests, differential tests against a reference book, and a market simulation. The simulation drives the engine with Hawkes-process order flow across several regimes (calm, near-critical bursts, a one-sided crash, a 1M-order deep book, a congested level, sweeps). It checks the engine against a reference model and an invariant ledger after every event. See [docs/TESTING.md](docs/TESTING.md) for the model, the measured statistics and the mutation results.

## Roadmap
1. ~~Correct core, project structure, tests~~
2. ~~More order types (Market, FillOrKill, GoodForDay) and reject reasons (`std::expected`)~~
3. ~~Performance: Google Benchmark harness, memory pool, intrusive lists, `std::flat_map` comparison, latency percentiles~~
4. Exchange: multiple symbols, one matching thread per symbol with lock-free SPSC queues, TCP gateway with a binary protocol, market-data feed, pre-trade risk checks
5. Simulation: market-maker and noise-trader bots, replay, visualisation
