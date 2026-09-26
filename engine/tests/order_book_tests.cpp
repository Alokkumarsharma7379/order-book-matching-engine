#include "test_support.hpp"

using namespace test_support;

TEST_CASE("Standalone book reports empty best prices", "[book]") {
    OrderBook book;
    CHECK_FALSE(book.best_bid());
    CHECK_FALSE(book.best_ask());
    CHECK(book.orders_at(Side::Buy, 100).empty());
    CHECK_FALSE(book.find_order(1));
    REQUIRE_NOTHROW(book.check_invariants());
}

TEST_CASE("Book snapshots order bids descending and asks ascending", "[book]") {
    OrderBook book;
    book.add_resting(resting(1, Side::Buy, 90, 2, 1));
    book.add_resting(resting(2, Side::Buy, 95, 3, 2));
    book.add_resting(resting(3, Side::Sell, 110, 4, 3));
    book.add_resting(resting(4, Side::Sell, 105, 5, 4));
    CHECK(book.best_bid() == 95);
    CHECK(book.best_ask() == 105);
    REQUIRE(book.bids().size() == 2);
    REQUIRE(book.asks().size() == 2);
    CHECK(book.bids()[0].price == 95);
    CHECK(book.bids()[1].price == 90);
    CHECK(book.asks()[0].price == 105);
    CHECK(book.asks()[1].price == 110);
    REQUIRE_NOTHROW(book.check_invariants());
}

TEST_CASE("Book queries return independent copies", "[book][query]") {
    OrderBook book;
    const auto original = resting(1, Side::Buy, 100, 5, 1);
    book.add_resting(original);
    auto copy = book.find_order(1).value();
    copy.remaining_quantity = 99;
    auto queue = book.orders_at(Side::Buy, 100);
    queue[0].price = 999;
    auto levels = book.bids();
    levels[0].total_quantity = 999;
    CHECK(order_fields(book.find_order(1).value()) == order_fields(original));
    CHECK(book.bids()[0].total_quantity == 5);
    REQUIRE_NOTHROW(book.check_invariants());
}

TEST_CASE("Book rejects invalid resting records without leaving levels", "[book][validation]") {
    const auto invalid_case = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
    OrderBook book;
    auto order = resting(1, Side::Buy, 100, 5, 1);
    switch (invalid_case) {
    case 0: order.id = 0; break;
    case 1: order.side = static_cast<Side>(99); break;
    case 2: order.type = OrderType::Market; order.price.reset(); break;
    case 3: order.price.reset(); break;
    case 4: order.price = 0; break;
    case 5: order.remaining_quantity = 0; break;
    case 6: order.remaining_quantity = -1; break;
    case 7: order.status = OrderStatus::Filled; break;
    case 8: order.status = OrderStatus::Cancelled; break;
    case 9: order.status = OrderStatus::PartiallyFilled; break;
    case 10: order.creation_sequence = 0; break;
    case 11: order.priority_sequence = 0; break;
    case 12: order.quantity_adjustment = std::numeric_limits<Quantity>::min(); break;
    }
    REQUIRE_THROWS_AS(book.add_resting(order), std::invalid_argument);
    CHECK(book.bids().empty());
    CHECK(book.asks().empty());
    CHECK_FALSE(book.find_order(1));
    REQUIRE_NOTHROW(book.check_invariants());
}

TEST_CASE("Book rejects duplicates stale priority and crossing insertion", "[book][validation]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    OrderBook book;
    const auto original = resting(1, side, 100, 5, 10);
    book.add_resting(original);
    REQUIRE_THROWS_AS(book.add_resting(original), std::invalid_argument);
    REQUIRE_THROWS_AS(book.add_resting(resting(2, side, 100, 5, 9)), std::invalid_argument);
    REQUIRE_THROWS_AS(book.add_resting(resting(2, opposite(side), 100, 5, 11)), std::invalid_argument);
    CHECK(book.active_order_count() == 1);
    CHECK(order_fields(book.find_order(1).value()) == order_fields(original));
    REQUIRE_NOTHROW(book.check_invariants());
}

TEST_CASE("Locations and FIFO remain valid as record storage grows", "[book][invariant]") {
    OrderBook book;
    for (OrderId id = 1; id <= 4096; ++id) {
        const auto side = id % 2 == 0 ? Side::Buy : Side::Sell;
        const Price price = (side == Side::Buy ? 90 : 110) + id % 5;
        book.add_resting(resting(id, side, price, 1, id));
    }
    CHECK(book.active_order_count() == 4096);
    for (const auto side : {Side::Buy, Side::Sell}) {
        const auto levels = side == Side::Buy ? book.bids() : book.asks();
        for (const auto& level : levels) {
            const auto orders = book.orders_at(side, level.price);
            CHECK(level.total_quantity == static_cast<Quantity>(orders.size()));
            SequenceNumber previous = 0;
            for (const auto& order : orders) {
                REQUIRE(order.priority_sequence > previous);
                REQUIRE(order_fields(book.find_order(order.id).value()) == order_fields(order));
                previous = order.priority_sequence;
            }
        }
    }
    REQUIRE_NOTHROW(book.check_invariants());
}
