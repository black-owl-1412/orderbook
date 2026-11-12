// Per-operation latency distribution over the mixed workload.
//   ./build-release/orderbook_latency [ops=1000000]
//
// Every call is timed individually with steady_clock. The clock read itself costs
// ~20 ns, so treat sub-50 ns numbers as "very fast" rather than exact; what matters
// here is the TAIL (p99 / p99.9 / max), which an average-throughput benchmark hides.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <print>
#include <string_view>
#include <utility>
#include <vector>

#include "Workload.h"
#include "orderbook/OrderBook.h"

using namespace orderbook;
using namespace orderbook::bench;
using Clock = std::chrono::steady_clock;

namespace {

enum Category : std::size_t { LimitAdd, MarketAdd, Cancel, Modify, CategoryCount };
constexpr std::array<std::string_view, CategoryCount> kNames{"limit add", "market add", "cancel",
                                                             "modify"};

Category Classify(const Op& op) {
    switch (op.kind) {
        case Op::Kind::Add: return op.type == OrderType::Market ? MarketAdd : LimitAdd;
        case Op::Kind::Cancel: return Cancel;
        case Op::Kind::Modify: return Modify;
    }
    std::unreachable();
}

std::int64_t Percentile(const std::vector<std::int64_t>& sorted, double p) {
    if (sorted.empty())
        return 0;
    const auto idx = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    const auto ops = GenerateMixedWorkload(count);

    {  // warm-up: page in allocator arenas, caches, branch predictors
        OrderBook warm;
        for (const Op& op : ops)
            Apply(warm, op);
    }

    std::array<std::vector<std::int64_t>, CategoryCount> samples;
    for (auto& s : samples)
        s.reserve(count);

    OrderBook book;
    for (const Op& op : ops) {
        const auto start = Clock::now();
        Apply(book, op);
        const auto stop = Clock::now();
        samples[Classify(op)].push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
    }

    std::println("{} ops, {} orders resting at end\n", ops.size(), book.Size());
    std::println("{:<11} {:>9} {:>7} {:>7} {:>7} {:>8} {:>9}", "op", "count", "p50", "p90", "p99",
                 "p99.9", "max (ns)");
    for (std::size_t c = 0; c < CategoryCount; ++c) {
        auto& s = samples[c];
        std::ranges::sort(s);
        std::println("{:<11} {:>9} {:>7} {:>7} {:>7} {:>8} {:>9}", kNames[c], s.size(),
                     Percentile(s, 0.50), Percentile(s, 0.90), Percentile(s, 0.99),
                     Percentile(s, 0.999), s.empty() ? 0 : s.back());
    }
    return 0;
}
