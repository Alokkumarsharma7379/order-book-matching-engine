#include "test_support.hpp"

#include <array>

using namespace test_support;

TEST_CASE("Empty engine queries have no liquidity or history", "[matching][empty]") {
    MatchingEngine engine;
    CHECK(engine.bids().empty());
    CHECK(engine.asks().empty());
    CHECK(engine.trades().empty());
    CHECK_FALSE(engine.find_order(1));
    CHECK(engine.active_order_count() == 0);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Limit buy rests without an ask", "[matching][limit]") {
    MatchingEngine engine;
    const auto result = limit(engine, Side::Buy, 10100, 50);
    CHECK(result.order.status == OrderStatus::New);
    CHECK(result.order.remaining_quantity == 50);
    CHECK(result.trades.empty());
    CHECK((level_fields(engine.bids()) == std::vector{std::tuple<Price, Quantity, std::size_t>{10100, 50, 1}}));
    CHECK(engine.asks().empty());
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Limit sell rests without a bid", "[matching][limit]") {
    MatchingEngine engine;
    const auto result = limit(engine, Side::Sell, 10100, 50);
    CHECK(result.order.status == OrderStatus::New);
    CHECK(result.order.remaining_quantity == 50);
    CHECK(result.trades.empty());
    REQUIRE(engine.asks().size() == 1);
    CHECK(engine.asks()[0].price == 10100);
    CHECK(engine.asks()[0].total_quantity == 50);
    CHECK(engine.bids().empty());
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Crossing buy executes at the resting sell price", "[matching][price]") {
    MatchingEngine engine;
    const auto maker = limit(engine, Side::Sell, 99, 5).order;
    const auto result = limit(engine, Side::Buy, 101, 5);
    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].price == 99);
    CHECK(result.trades[0].maker_order_id == maker.id);
    CHECK(result.trades[0].taker_order_id == result.order.id);
    CHECK(result.order.status == OrderStatus::Filled);
    CHECK(engine.find_order(maker.id)->status == OrderStatus::Filled);
    CHECK(engine.active_order_count() == 0);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Crossing sell executes at the resting buy price", "[matching][price]") {
    MatchingEngine engine;
    const auto maker = limit(engine, Side::Buy, 101, 5).order;
    const auto result = limit(engine, Side::Sell, 99, 5);
    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].price == 101);
    CHECK(result.trades[0].maker_order_id == maker.id);
    CHECK(result.order.status == OrderStatus::Filled);
    CHECK(engine.active_order_count() == 0);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Best price outranks arrival time on either side", "[matching][priority]") {
    const auto incoming = GENERATE(Side::Buy, Side::Sell);
    MatchingEngine engine;
    const Price worse = incoming == Side::Buy ? 102 : 98;
    (void)limit(engine, opposite(incoming), worse, 5);
    const auto best = limit(engine, opposite(incoming), 100, 5).order;
    const auto result = market(engine, incoming, 2);
    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].maker_order_id == best.id);
    CHECK(result.trades[0].price == 100);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("FIFO survives equal and backward timestamps and partial fills", "[matching][fifo]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    const auto later_clock = GENERATE(10, -10);
    MatchingEngine engine;
    auto request = limit_request(side, 100, 4);
    const auto first = engine.submit(request).order;
    request.timestamp = stamp(later_clock);
    const auto second = engine.submit(request).order;
    const auto result = market(engine, opposite(side), 6);
    REQUIRE(result.trades.size() == 2);
    CHECK(result.trades[0].maker_order_id == first.id);
    CHECK(result.trades[0].quantity == 4);
    CHECK(result.trades[1].maker_order_id == second.id);
    CHECK(result.trades[1].quantity == 2);
    CHECK(engine.find_order(second.id)->priority_sequence == second.priority_sequence);
    CHECK(engine.find_order(second.id)->status == OrderStatus::PartiallyFilled);
    const auto next = market(engine, opposite(side), 1);
    REQUIRE(next.trades.size() == 1);
    CHECK(next.trades[0].maker_order_id == second.id);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Partial maker fill updates quantity and cached total", "[matching][partial]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    MatchingEngine engine;
    const auto maker = limit(engine, side, 100, 8).order;
    const auto result = market(engine, opposite(side), 3);
    CHECK(result.order.status == OrderStatus::Filled);
    const auto record = engine.find_order(maker.id).value();
    CHECK(record.remaining_quantity == 5);
    CHECK(record.executed_quantity == 3);
    CHECK(record.original_quantity == 8);
    CHECK(record.updated_at == stamp(20));
    const auto levels = side == Side::Buy ? engine.bids() : engine.asks();
    REQUIRE(levels.size() == 1);
    CHECK(levels[0].total_quantity == 5);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Incoming limit remainder rests and can later become a maker", "[matching][partial]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    MatchingEngine engine;
    (void)limit(engine, opposite(side), 100, 5);
    const auto partial = limit(engine, side, 100, 8);
    CHECK(partial.order.status == OrderStatus::PartiallyFilled);
    CHECK(partial.order.remaining_quantity == 3);
    CHECK(partial.order.executed_quantity == 5);
    const auto next = market(engine, opposite(side), 3);
    REQUIRE(next.trades.size() == 1);
    CHECK(next.trades[0].maker_order_id == partial.order.id);
    CHECK(engine.find_order(partial.order.id)->executed_quantity == 8);
    CHECK(engine.find_order(partial.order.id)->status == OrderStatus::Filled);
    CHECK(engine.bids().empty());
    CHECK(engine.asks().empty());
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("One taker fills several orders at an identical price", "[matching][sweep]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    MatchingEngine engine;
    for (int i = 0; i < 5; ++i) {
        (void)limit(engine, opposite(side), 100, 2);
    }
    const auto result = market(engine, side, 10);
    REQUIRE(result.trades.size() == 5);
    for (std::size_t i = 0; i < result.trades.size(); ++i) {
        CHECK(result.trades[i].maker_order_id == static_cast<OrderId>(i + 1));
        CHECK(result.trades[i].quantity == 2);
    }
    CHECK(engine.active_order_count() == 0);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Multi-level sweep produces the required 20 30 50 fills", "[matching][sweep]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    MatchingEngine engine;
    const auto prices = side == Side::Buy ? std::array{99, 100, 101} : std::array{101, 100, 99};
    (void)limit(engine, opposite(side), prices[0], 20);
    (void)limit(engine, opposite(side), prices[1], 30);
    (void)limit(engine, opposite(side), prices[2], 70);
    const auto result = limit(engine, side, prices[2], 100);
    REQUIRE(result.trades.size() == 3);
    CHECK(result.trades[0].quantity == 20);
    CHECK(result.trades[1].quantity == 30);
    CHECK(result.trades[2].quantity == 50);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(result.trades[i].price == prices[i]);
    }
    CHECK(engine.find_order(3)->remaining_quantity == 20);
    CHECK(result.order.status == OrderStatus::Filled);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Limit orders stop at an unacceptable price", "[matching][limit]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    MatchingEngine engine;
    (void)limit(engine, opposite(side), 100, 2);
    const auto untouched = limit(engine, opposite(side), side == Side::Buy ? 101 : 99, 4).order;
    const auto result = limit(engine, side, 100, 5);
    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].quantity == 2);
    CHECK(result.order.remaining_quantity == 3);
    CHECK(order_fields(engine.find_order(untouched.id).value()) == order_fields(untouched));
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Market buy consumes available asks", "[matching][market]") {
    MatchingEngine engine;
    (void)limit(engine, Side::Sell, 100, 5);
    const auto result = market(engine, Side::Buy, 5);
    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].quantity == 5);
    CHECK_FALSE(result.order.price);
    CHECK(result.order.status == OrderStatus::Filled);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Market sell consumes available bids", "[matching][market]") {
    MatchingEngine engine;
    (void)limit(engine, Side::Buy, 100, 5);
    const auto result = market(engine, Side::Sell, 5);
    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].quantity == 5);
    CHECK_FALSE(result.order.price);
    CHECK(result.order.status == OrderStatus::Filled);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Insufficient market liquidity cancels only the unexecuted remainder", "[matching][market]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    const auto liquidity = GENERATE(Quantity{0}, Quantity{3});
    MatchingEngine engine;
    if (liquidity != 0) {
        (void)limit(engine, opposite(side), 100, liquidity);
    }
    const auto result = market(engine, side, 8);
    CHECK(result.order.status == OrderStatus::Cancelled);
    CHECK(result.order.executed_quantity == liquidity);
    CHECK(result.order.remaining_quantity == 8 - liquidity);
    CHECK(engine.active_order_count() == 0);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("IDs sequences and timestamps describe actual executions", "[matching][metadata]") {
    MatchingEngine engine;
    (void)limit(engine, Side::Sell, 100, 2);
    (void)limit(engine, Side::Sell, 100, 2);
    const auto result = market(engine, Side::Buy, 4);
    CHECK(result.order.id == 3);
    CHECK(result.order.creation_sequence == 3);
    REQUIRE(result.trades.size() == 2);
    CHECK(result.trades[0].id == 1);
    CHECK(result.trades[0].execution_sequence == 4);
    CHECK(result.trades[1].id == 2);
    CHECK(result.trades[1].execution_sequence == 5);
    CHECK(result.trades[0].executed_at == stamp(20));
    CHECK(engine.find_order(1)->updated_at == stamp(20));
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Trade pagination and returned snapshots do not expose mutable state", "[query]") {
    MatchingEngine engine;
    for (int i = 0; i < 3; ++i) {
        (void)limit(engine, Side::Sell, 100, 1);
    }
    (void)market(engine, Side::Buy, 3);
    auto page = engine.trades(1, 1);
    REQUIRE(page.size() == 1);
    CHECK(page[0].id == 2);
    page[0].quantity = 999;
    CHECK(engine.trades(1, 1)[0].quantity == 1);
    CHECK(engine.trades(3).empty());
    CHECK(engine.trades(0, 0).empty());
    REQUIRE_THROWS_AS(engine.trades(-1), std::invalid_argument);
    auto order = engine.find_order(1).value();
    order.remaining_quantity = 999;
    CHECK(engine.find_order(1)->remaining_quantity == 0);
    REQUIRE_NOTHROW(engine.check_invariants());
}
