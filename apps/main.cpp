#include <memory>
#include <print>
#include <utility>

#include "orderbook/OrderBook.h"
#include "orderbook/ToString.h"

using namespace orderbook;

namespace {

void PrintResult(const AddResult& result) {
    if (!result) {
        std::println("  REJECTED: {}", ToString(result.error()));
        return;
    }
    if (result->empty())
        std::println("  accepted (no trades)");
    for (const auto& t : *result)
        std::println("  TRADE bid#{} x ask#{}  {} @ {}", t.GetBidTrade().orderId_,
                     t.GetAskTrade().orderId_, t.GetQuantity(), t.GetPrice());
}

void PrintBook(const OrderBook& book) {
    const auto infos = book.GetOrderInfos();
    std::println("  ---- book ({} orders) ----", book.Size());
    std::println("  {:>8} | {:<6} | {:<8}", "bid qty", "price", "ask qty");
    // Asks printed highest-first so the spread sits in the middle.
    const auto& asks = infos.GetAsks();
    for (auto it = asks.rbegin(); it != asks.rend(); ++it)
        std::println("  {:>8} | {:<6} | {:<8}", "", it->price_, it->quantity_);
    for (const auto& b : infos.GetBids())
        std::println("  {:>8} | {:<6} | {:<8}", b.quantity_, b.price_, "");
    std::println("");
}

AddResult Submit(OrderBook& book, OrderType type, OrderId id, Side side, Price price, Quantity qty) {
    if (type == OrderType::Market)
        std::println("> #{} {} {} {}  (type id {})", id, ToString(type), ToString(side), qty,
                     std::to_underlying(type));
    else
        std::println("> #{} {} {} {} @ {}  (type id {})", id, ToString(type), ToString(side), qty,
                     price, std::to_underlying(type));
    return book.AddOrder(std::make_shared<Order>(type, id, side, qty, price));
}

}  // namespace

int main() {
    OrderBook book;

    std::println("== Build a book ==");
    Submit(book, OrderType::GoodTillCancel, 1, Side::Buy, 99, 10);
    Submit(book, OrderType::GoodForDay, 2, Side::Buy, 100, 5);
    Submit(book, OrderType::GoodTillCancel, 3, Side::Sell, 101, 7);
    Submit(book, OrderType::GoodForDay, 4, Side::Sell, 102, 8);
    Submit(book, OrderType::GoodTillCancel, 5, Side::Sell, 103, 10);
    PrintBook(book);

    std::println("== FillOrKill buy 20 @ 102: only 15 available up to 102 -> rejected ==");
    PrintResult(Submit(book, OrderType::FillOrKill, 6, Side::Buy, 102, 20));
    PrintBook(book);

    std::println("== FillOrKill buy 10 @ 102: enough liquidity -> fills fully ==");
    PrintResult(Submit(book, OrderType::FillOrKill, 7, Side::Buy, 102, 10));
    PrintBook(book);

    std::println("== Market sell 15: sweeps all bids at any price ==");
    PrintResult(Submit(book, OrderType::Market, 8, Side::Sell, 0, 15));
    PrintBook(book);

    std::println("== Market sell 1: no bids left -> rejected ==");
    PrintResult(Submit(book, OrderType::Market, 9, Side::Sell, 0, 1));
    std::println("");

    std::println("== Duplicate id / cancel unknown ==");
    PrintResult(Submit(book, OrderType::GoodTillCancel, 5, Side::Buy, 90, 1));
    if (auto r = book.CancelOrder(999); !r)
        std::println("  cancel #999 REJECTED: {}", ToString(r.error()));
    std::println("");

    std::println("== End of day: GoodForDay orders expire ==");
    for (const OrderId id : book.CancelGoodForDayOrders())
        std::println("  expired #{}", id);
    PrintBook(book);

    return 0;
}
