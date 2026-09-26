#include "orderbook/order_book.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace orderbook;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function>
void expect_failure(Function action) {
    try {
        action();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error("expected rejection did not occur");
}

Order resting(OrderId id, Side side, Price price, Quantity quantity, SequenceNumber sequence) {
    return Order{
        .id = id, .side = side, .type = OrderType::Limit, .price = price,
        .original_quantity = quantity, .remaining_quantity = quantity,
        .executed_quantity = 0, .creation_sequence = sequence,
        .priority_sequence = sequence,
        .created_at = Timestamp{std::chrono::microseconds{sequence}},
        .updated_at = Timestamp{std::chrono::microseconds{sequence}},
        .status = OrderStatus::New
    };
}

void check_storage() {
    OrderBook book;
    require(!book.best_bid() && !book.best_ask() && book.active_order_count() == 0,
            "new book is not empty");
    require(book.bids().empty() && book.asks().empty() && !book.find_order(999),
            "empty queries failed");
    book.check_invariants();

    book.add_resting(resting(1, Side::Buy, 10'000, 10, 1));
    book.add_resting(resting(2, Side::Buy, 10'100, 20, 2));
    book.add_resting(resting(3, Side::Buy, 10'100, 30, 3));
    book.add_resting(resting(4, Side::Sell, 10'300, 40, 4));
    book.add_resting(resting(5, Side::Sell, 10'200, 50, 5));
    require(book.best_bid() == 10'100 && book.best_ask() == 10'200, "wrong best prices");
    const auto bids = book.bids();
    const auto asks = book.asks();
    require(bids.size() == 2 && bids[0].price == 10'100 && bids[1].price == 10'000
            && bids[0].total_quantity == 50 && bids[0].order_count == 2,
            "wrong bid snapshot");
    require(asks.size() == 2 && asks[0].price == 10'200 && asks[1].price == 10'300,
            "wrong ask ordering");
    auto queue = book.orders_at(Side::Buy, 10'100);
    require(queue.size() == 2 && queue[0].id == 2 && queue[1].id == 3, "wrong FIFO");
    queue[0].remaining_quantity = 1;
    auto copy = book.find_order(2).value();
    copy.price = 1;
    require(copy.price == 1 && book.find_order(2)->remaining_quantity == 20
            && book.find_order(2)->price == 10'100,
            "snapshot mutation changed the book");
    require(book.orders_at(Side::Sell, 99).empty(), "missing level should return empty");

    expect_failure<std::invalid_argument>([&] { book.add_resting(resting(1, Side::Buy, 99, 1, 6)); });
    expect_failure<std::invalid_argument>([&] { book.add_resting(resting(6, Side::Buy, 10'100, 1, 2)); });
    expect_failure<std::invalid_argument>([&] { book.add_resting(resting(6, Side::Buy, 10'200, 1, 6)); });
    expect_failure<std::invalid_argument>([&] { book.add_resting(resting(6, Side::Sell, 10'100, 1, 6)); });
    for (const Quantity quantity : {Quantity{0}, Quantity{-1}}) {
        expect_failure<std::invalid_argument>([&] { book.add_resting(resting(6, Side::Buy, 99, quantity, 6)); });
    }
    auto market = resting(6, Side::Buy, 99, 1, 6);
    market.type = OrderType::Market;
    market.price.reset();
    expect_failure<std::invalid_argument>([&] { book.add_resting(market); });
    require(book.active_order_count() == 5 && !book.find_order(6), "rejection changed state");
    book.check_invariants();

    for (OrderId id = 6; id < 2054; ++id) {
        book.add_resting(resting(id, Side::Buy, 9'000 + id % 64, 1, id));
    }
    book.check_invariants();
    require(book.active_order_count() == 2053, "incorrect count after growth");
    require(book.orders_at(Side::Buy, 10'100)[0].id == 2, "growth damaged existing FIFO");
    for (OrderId id = 6; id < 2054; ++id) {
        const auto order = book.find_order(id);
        require(order && order->price == 9'000 + id % 64, "lookup failed after growth");
    }
}

void check_quantity_boundary() {
    OrderBook book;
    constexpr Quantity maximum = std::numeric_limits<Quantity>::max();
    book.add_resting(resting(1, Side::Buy, 100, maximum, 1));
    expect_failure<std::overflow_error>([&] { book.add_resting(resting(2, Side::Buy, 100, 1, 2)); });
    require(book.active_order_count() == 1 && book.bids()[0].total_quantity == maximum,
            "overflow rejection changed state");
    book.add_resting(resting(2, Side::Buy, 99, 1, 2));
    book.check_invariants();
}

} // namespace

int main() {
    try {
        check_storage();
        check_quantity_boundary();
        std::cout << "OrderBook storage checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << "OrderBook storage check failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
