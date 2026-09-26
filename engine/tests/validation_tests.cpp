#include "test_support.hpp"

using namespace test_support;

TEST_CASE("Zero negative and minimum signed quantities are rejected without mutation", "[validation]") {
    const auto quantity = GENERATE(Quantity{0}, Quantity{-1}, std::numeric_limits<Quantity>::min());
    const auto type = GENERATE(OrderType::Limit, OrderType::Market);
    MatchingEngine engine;
    auto request = limit_request(Side::Buy, 100, quantity);
    request.type = type;
    if (type == OrderType::Market) {
        request.price.reset();
    }
    const auto before = state(engine, {1});
    REQUIRE_THROWS_AS(engine.submit(request), std::invalid_argument);
    CHECK(state(engine, {1}) == before);
    const auto next = limit(engine, Side::Buy, 100, 1).order;
    CHECK(next.id == 1);
    CHECK(next.creation_sequence == 1);
}

TEST_CASE("Limit price is required and must be positive", "[validation]") {
    const auto price = GENERATE(std::optional<Price>{}, std::optional<Price>{0},
        std::optional<Price>{-1}, std::optional<Price>{std::numeric_limits<Price>::min()});
    MatchingEngine engine;
    auto request = limit_request(Side::Buy, 100, 1);
    request.price = price;
    REQUIRE_THROWS_AS(engine.submit(request), std::invalid_argument);
    CHECK(engine.active_order_count() == 0);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Market orders reject supplied prices", "[validation]") {
    const auto price = GENERATE(Price{-1}, Price{0}, Price{100});
    MatchingEngine engine;
    SubmitOrder request{Side::Buy, OrderType::Market, price, 1, stamp(0), std::nullopt};
    REQUIRE_THROWS_AS(engine.submit(request), std::invalid_argument);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Invalid enum values and nonpositive IDs are rejected", "[validation]") {
    MatchingEngine engine;
    auto request = limit_request(Side::Buy, 100, 1);
    SECTION("side") { request.side = static_cast<Side>(123); }
    SECTION("type") { request.type = static_cast<OrderType>(123); }
    SECTION("zero ID") { request.id = 0; }
    SECTION("negative ID") { request.id = -1; }
    REQUIRE_THROWS_AS(engine.submit(request), std::invalid_argument);
    CHECK(engine.healthy());
    CHECK(engine.active_order_count() == 0);
}

TEST_CASE("Duplicate IDs are rejected for active filled and cancelled orders", "[validation][ids]") {
    const auto disposition = GENERATE(0, 1, 2);
    MatchingEngine engine;
    const auto original = engine.submit(limit_request(Side::Buy, 100, 2, 1)).order;
    if (disposition == 1) {
        (void)market(engine, Side::Sell, 2);
    } else if (disposition == 2) {
        (void)engine.cancel(original.id, stamp(30));
    }
    const auto before = state(engine, {1, 2, 3});
    REQUIRE_THROWS_AS(engine.submit(limit_request(Side::Buy, 99, 1, 1)), DuplicateOrderId);
    CHECK(state(engine, {1, 2, 3}) == before);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Explicit IDs advance generation without preventing unused lower IDs", "[validation][ids]") {
    MatchingEngine engine;
    CHECK(engine.submit(limit_request(Side::Buy, 100, 1, 100)).order.id == 100);
    CHECK(limit(engine, Side::Buy, 100, 1).order.id == 101);
    CHECK(engine.submit(limit_request(Side::Buy, 100, 1, 7)).order.id == 7);
    CHECK(limit(engine, Side::Buy, 100, 1).order.id == 102);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Automatic order ID exhaustion does not overflow or change state", "[validation][boundary]") {
    MatchingEngine engine;
    constexpr auto maximum = std::numeric_limits<OrderId>::max();
    (void)engine.submit(limit_request(Side::Buy, 100, 1, maximum));
    const auto before = state(engine, {maximum});
    REQUIRE_THROWS_AS(limit(engine, Side::Buy, 99, 1), std::overflow_error);
    CHECK(state(engine, {maximum}) == before);
    CHECK(engine.submit(limit_request(Side::Buy, 99, 1, 7)).order.creation_sequence == 2);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Maximum quantity and price match without notional multiplication", "[validation][boundary]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    MatchingEngine engine;
    constexpr Quantity quantity = std::numeric_limits<Quantity>::max();
    constexpr Price price = std::numeric_limits<Price>::max();
    (void)limit(engine, opposite(side), price, quantity);
    const auto result = limit(engine, side, price, quantity);
    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].quantity == quantity);
    CHECK(result.trades[0].price == price);
    CHECK(result.order.status == OrderStatus::Filled);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Level overflow on submission or repricing preserves the original state", "[validation][boundary]") {
    MatchingEngine engine;
    constexpr Quantity maximum = std::numeric_limits<Quantity>::max();
    (void)limit(engine, Side::Buy, 100, maximum);
    const auto movable = limit(engine, Side::Buy, 99, 1).order;
    const auto before = state(engine, {1, 2, 3});
    REQUIRE_THROWS_AS(limit(engine, Side::Buy, 100, 1), std::overflow_error);
    REQUIRE_THROWS_AS(engine.modify(movable.id, {100, std::nullopt, stamp(30)}), std::overflow_error);
    CHECK(state(engine, {1, 2, 3}) == before);
    CHECK(limit(engine, Side::Buy, 98, 1).order.creation_sequence == 3);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Same-level amendment subtracts the old quantity before checking capacity", "[validation][boundary]") {
    MatchingEngine engine;
    constexpr Quantity maximum = std::numeric_limits<Quantity>::max();
    const auto original = limit(engine, Side::Buy, 100, maximum - 1).order;
    const auto result = engine.modify(original.id, {std::nullopt, maximum, stamp(30)});
    CHECK(result.order.remaining_quantity == maximum);
    CHECK(engine.bids()[0].total_quantity == maximum);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Amendment bounds include quantity already executed", "[validation][boundary]") {
    MatchingEngine engine;
    constexpr Quantity maximum = std::numeric_limits<Quantity>::max();
    const auto original = limit(engine, Side::Buy, 100, 2).order;
    (void)market(engine, Side::Sell, 1);
    const auto before = state(engine, {1, 2});
    REQUIRE_THROWS_AS(engine.modify(original.id, {std::nullopt, maximum, stamp(30)}), std::overflow_error);
    CHECK(state(engine, {1, 2}) == before);
    (void)engine.modify(original.id, {std::nullopt, maximum - 1, stamp(30)});
    (void)market(engine, Side::Sell, maximum - 1);
    CHECK(engine.find_order(original.id)->executed_quantity == maximum);
    CHECK(engine.find_order(original.id)->quantity_adjustment == maximum - 2);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Invalid amendment values and IDs leave orders unchanged", "[validation][modify]") {
    MatchingEngine engine;
    const auto original = limit(engine, Side::Buy, 100, 5).order;
    const auto before = state(engine, {1, 2});
    REQUIRE_THROWS_AS(engine.modify(original.id, {}), std::invalid_argument);
    for (const auto value : {std::int64_t{0}, std::int64_t{-1}, std::numeric_limits<std::int64_t>::min()}) {
        CAPTURE(value);
        REQUIRE_THROWS_AS(engine.modify(original.id, {value, std::nullopt, stamp(30)}), std::invalid_argument);
        REQUIRE_THROWS_AS(engine.modify(original.id, {std::nullopt, value, stamp(30)}), std::invalid_argument);
        REQUIRE_THROWS_AS(engine.modify(value, {101, 1, stamp(30)}), std::invalid_argument);
        REQUIRE_THROWS_AS(engine.cancel(value, stamp(30)), std::invalid_argument);
    }
    CHECK(state(engine, {1, 2}) == before);
    CHECK(limit(engine, Side::Buy, 99, 1).order.creation_sequence == 2);
    REQUIRE_NOTHROW(engine.check_invariants());
}
