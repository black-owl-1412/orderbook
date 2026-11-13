# Performance

Every number here was measured. Six versions of the engine were built in Release (`-O3`) and run back to back on the same machine, pinned to one core, against the same benchmarks. Throughput figures are the median of 7 repetitions. The raw output is in `bench/results/`.

Machine: Intel Core i7-1165G7 (laptop), WSL2 Ubuntu, GCC 15.2. Timings on this setup vary by 5–20% between runs, so treat smaller differences as noise.

## Summary: original engine vs current

| Benchmark | Original | Current | Change |
|---|---|---|---|
| Mixed workload, 1M operations | 185 ms (5.4M ops/s) | 82.2 ms (12.2M ops/s) | 2.3× faster |
| Add 100k resting orders | 11.6 ms | 5.37 ms | 2.2× faster |
| Cancel 100k resting orders | 28.2 ms | 12.8 ms | 2.2× faster |
| Market order sweeping 1,000 orders (10 levels) | 64.3 µs | 24.9 µs | 2.6× faster |
| Market order sweeping 1,000 orders (100 levels) | 75.1 µs | 27.0 µs | 2.8× faster |
| Depth snapshot, 20k orders on 200 levels | 185 µs | 0.147 µs | about 1,260× faster |
| FillOrKill reject, 20k orders on 200 levels | 82.6 µs | 0.070 µs | about 1,180× faster |

Latency per operation over the mixed workload, in nanoseconds:

| Operation | p50 | p99 | p99.9 |
|---|---|---|---|
| Limit add | 166 → 93 | 1,131 → 331 | 2,907 → 602 |
| Market add | 370 → 123 | 1,480 → 384 | 4,403 → 664 |
| Cancel | 138 → 83 | 780 → 246 | 3,177 → 556 |
| Modify | 152 → 89 | 1,283 → 412 | 3,576 → 1,718 |

Each call is timed with `steady_clock`, whose own cost is around 20 ns, so the small numbers are approximate. The maximum latencies (15–600 µs) are dominated by the operating system and by container growth, and vary from run to run.

## Step by step

| Version | Mixed 1M ops | Add 100k | Cancel 100k | Sweep 10×100 | Snapshot 20k | FOK reject 20k |
|---|---|---|---|---|---|---|
| 0. Original (`shared_ptr` + `std::list`) | 185 ms | 11.6 ms | 28.2 ms | 64.3 µs | 185 µs | 82.6 µs |
| 1. Running total per price level | 193 ms | 9.56 ms | 32.1 ms | 71.4 µs | 1.23 µs | 0.622 µs |
| 2. Object pool + intrusive lists | 100 ms | 4.53 ms | 14.5 ms | 25.1 µs | 0.999 µs | 0.512 µs |
| 3. Match before insert | 85.9 ms | 4.78 ms | 14.1 ms | 23.5 µs | 1.08 µs | 0.507 µs |
| 4. Best level stored last, `std::map` | 96.6 ms | 4.94 ms | 14.1 ms | 24.9 µs | 1.21 µs | 0.475 µs |
| 5. Best level stored last, `std::flat_map` (current) | 82.2 ms | 5.37 ms | 12.8 ms | 24.9 µs | 0.147 µs | 0.070 µs |

### 1. Running total per price level
Each level keeps the sum of its resting quantity, updated on every add, fill and cancel. A depth snapshot and the FillOrKill check used to visit every order; now they visit every level. On a book with 20k orders this is about 150× faster. Add, cancel and matching are unchanged within noise.

### 2. Object pool and intrusive lists
The original stored each order as a `shared_ptr<Order>` inside a `std::list`: two heap allocations per order, plus reference counting. Now each resting order is one node from `ObjectPool`, and the node itself holds the links to its queue neighbours. Adding or removing an order allocates nothing for the order. This is the largest single gain: the mixed workload goes from 193 ms to 100 ms, and tail latency drops by more than half (limit add p99 1,094 → 396 ns).

Under AddressSanitizer the pool marks freed slots as unreadable, so a use-after-free is still reported even though the memory is never returned to the system.

### 3. Match before insert
The original inserted every incoming order into the book and then matched the two best levels against each other. An order that filled immediately was allocated, indexed, and removed again. Now the incoming order trades against the other side first and only its remainder is inserted. Market order median latency drops from 189 to 130 ns, and the mixed workload from 100 ms to 85.9 ms. The special step that cancelled leftover immediate-or-cancel orders is gone.

### 4 and 5. Level container
Each side is sorted so that its best price is the last element. Levels near the best price are the ones created and removed most often, and removing the last element of a sorted array is cheap.

With `std::map` this layout measures 96.6 ms on the mixed workload against 85.9 ms for the previous layout. That is within run-to-run noise, so no gain or loss can be claimed for the layout alone.

With `std::flat_map` (a sorted array) the mixed workload measures 82.2 ms, and walking the levels is about 7× faster than with `std::map`: snapshot 1.21 → 0.147 µs, FillOrKill reject 0.475 → 0.070 µs. Adding 100k resting orders measures about 9% slower (4.94 → 5.37 ms), which is inside the noise range.

`std::flat_map` is the default. Build with `-DORDERBOOK_FLAT_LEVELS=OFF` to use `std::map`.

## Limits of these results
- **A sorted array pays to insert or remove a level far from the best price**: every level between that point and the best price is shifted. The benchmarks use at most a few hundred levels per side, where this is cheap. A book with many thousands of sparse levels and activity far from the best price is not measured here and could favour `std::map`.
- **The id index is still `std::unordered_map`**, which allocates one node per resting order. It is the main remaining allocation on the add and cancel paths.
- **`AddOrder` returns a `std::vector` of trades**, which allocates when an order trades.
- **Single-threaded, one instrument, synthetic flow.** The mixed workload is 50% passive adds, 30% cancels, 10% aggressive limits, 5% market orders and 5% modifies.
- **A laptop under WSL2 is not a quiet benchmark machine.** No core isolation or frequency pinning was used beyond `taskset`.

## Reproducing
```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DORDERBOOK_BUILD_TESTS=OFF
cmake --build build-release -j
taskset -c 3 ./build-release/orderbook_bench --benchmark_repetitions=7 --benchmark_report_aggregates_only=true
taskset -c 3 ./build-release/orderbook_latency
```
