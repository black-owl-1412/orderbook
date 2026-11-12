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
| `std::map<Price, std::list<OrderPointer>>` per side (bids descending, asks ascending) | price levels, each a FIFO queue | O(log L) to find or insert a level, O(1) to reach the best level |
| `std::unordered_map<OrderId, {order, list iterator}>` | look up an order by id | O(1) average cancel |

`std::list` iterators stay valid when other elements are inserted or removed. That is why each order can store its own position and be cancelled in O(1).

This is the correctness baseline. A later performance milestone will replace it with pooled orders, intrusive lists and flat price arrays, and benchmark them against this version.

## Layout
```
include/orderbook/   public headers (Types, Order, OrderModify, Trade, LevelInfo, OrderBook)
src/                 matching engine implementation
apps/main.cpp        demo
tests/               GoogleTest suite
legacy/main_v0.cpp   original single-file version (not built)
```

## Build and test
Requires CMake ≥ 3.28 and a C++23 compiler (tested with GCC 15). GoogleTest comes from the system if installed; otherwise CMake downloads it.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DORDERBOOK_SANITIZE=ON   # ASan + UBSan
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/orderbook_demo
```

## Roadmap
1. ~~Correct core, project structure, tests~~
2. ~~More order types (Market, FillOrKill, GoodForDay) and reject reasons (`std::expected`)~~
3. Performance: Google Benchmark harness, memory pool, intrusive lists, `std::flat_map` comparison, latency histograms
4. Exchange: multiple symbols, one matching thread per symbol with lock-free SPSC queues, TCP gateway with a binary protocol, market-data feed, pre-trade risk checks
5. Simulation: market-maker and noise-trader bots, replay, visualisation
