#include <memory>
#include <print>
#include <string_view>
#include <utility>

#include "orderbook/OrderBook.h"

using namespace orderbook;

namespace {

std::string_view ToString(Side side) {
    switch (side) {
        case Side::Buy: return "BUY";
        case Side::Sell: return "SELL";
    }
    std::unreachable();  // every enumerator is handled above
}

std::string_view ToString(OrderType type) {
    switch (type) {
        case OrderType::GoodTillCancel: return "GTC";
        case OrderType::FillAndKill: return "FAK";
    }
    std::unreachable();
}

void PrintTrades(const Trades& trades) {
    if (trades.empty())
        std::println("  (no trades)");
    for (const auto& t : trades)
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

Trades Submit(OrderBook& book, OrderType type, OrderId id, Side side, Price price, Quantity qty) {
    std::println("> #{} {} {} {} @ {}  (type id {})", id, ToString(type), ToString(side), qty,
                 price, std::to_underlying(type));
    return book.AddOrder(std::make_shared<Order>(type, id, side, qty, price));
}

}  // namespace

int main() {
    OrderBook book;

    std::println("== Build a book ==");
    Submit(book, OrderType::GoodTillCancel, 1, Side::Buy, 99, 10);
    Submit(book, OrderType::GoodTillCancel, 2, Side::Buy, 100, 5);
    Submit(book, OrderType::GoodTillCancel, 3, Side::Sell, 101, 7);
    Submit(book, OrderType::GoodTillCancel, 4, Side::Sell, 102, 8);
    PrintBook(book);

    std::println("== Aggressive buy sweeps two ask levels ==");
    PrintTrades(Submit(book, OrderType::GoodTillCancel, 5, Side::Buy, 102, 10));
    PrintBook(book);

    std::println("== FillAndKill sell: fills 15, remaining 5 is killed ==");
    PrintTrades(Submit(book, OrderType::FillAndKill, 6, Side::Sell, 99, 20));
    PrintBook(book);

    std::println("== Cancel #4 ==");
    book.CancelOrder(4);
    PrintBook(book);

    return 0;
}
