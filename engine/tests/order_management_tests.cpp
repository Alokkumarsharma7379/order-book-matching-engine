#include "test_support.hpp"

using namespace test_support;

TEST_CASE("Cancellation removes head middle or tail without disturbing FIFO", "[cancel]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    const auto removed = GENERATE(OrderId{1}, OrderId{2}, OrderId{3});
    MatchingEngine engine;
    for (int i = 0; i < 3; ++i) {
        (void)limit(engine, side, 100, 2);
    }
    const auto old = engine.find_order(removed).value();
    const auto cancelled = engine.cancel(removed, stamp(30));
    CHECK(cancelled.status == OrderStatus::Cancelled);
    CHECK(cancelled.remaining_quantity == 2);
    CHECK(cancelled.updated_at == stamp(30));
    CHECK(cancelled.priority_sequence == old.priority_sequence);
    const auto result = market(engine, opposite(side), 4);
    REQUIRE(result.trades.size() == 2);
    OrderId previous = 0;
    for (const auto& trade : result.trades) {
        CHECK(trade.maker_order_id != removed);
        CHECK(trade.maker_order_id > previous);
        previous = trade.maker_order_id;
    }
    CHECK(engine.active_order_count() == 0);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Cancelling the sole best order reveals the next price", "[cancel]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    MatchingEngine engine;
    const Price worse = side == Side::Buy ? 99 : 101;
    (void)limit(engine, side, worse, 2);
    const auto best = limit(engine, side, 100, 3).order;
    (void)engine.cancel(best.id, stamp(30));
    const auto levels = side == Side::Buy ? engine.bids() : engine.asks();
    REQUIRE(levels.size() == 1);
    CHECK(levels[0].price == worse);
    CHECK(levels[0].total_quantity == 2);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Cancelling a partially filled order preserves execution history", "[cancel][partial]") {
    MatchingEngine engine;
    const auto order = limit(engine, Side::Buy, 100, 8).order;
    (void)market(engine, Side::Sell, 3);
    const auto cancelled = engine.cancel(order.id, stamp(30));
    CHECK(cancelled.executed_quantity == 3);
    CHECK(cancelled.remaining_quantity == 5);
    CHECK(cancelled.original_quantity == 8);
    REQUIRE(engine.trades().size() == 1);
    CHECK(engine.trades()[0].quantity == 3);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Unknown and inactive orders reject cancellation and amendment", "[cancel][modify][errors]") {
    MatchingEngine engine;
    REQUIRE_THROWS_AS(engine.cancel(99, stamp(0)), UnknownOrderId);
    REQUIRE_THROWS_AS(engine.modify(99, {101, 1, stamp(0)}), UnknownOrderId);
    const auto disposition = GENERATE(0, 1, 2);
    OrderId id = 0;
    if (disposition == 2) {
        id = market(engine, Side::Buy, 1).order.id;
    } else {
        id = limit(engine, Side::Buy, 100, 1).order.id;
        if (disposition == 0) {
            (void)engine.cancel(id, stamp(30));
        } else {
            (void)market(engine, Side::Sell, 1);
        }
    }
    const auto before = state(engine, {1, 2});
    REQUIRE_THROWS_AS(engine.cancel(id, stamp(40)), OrderNotActive);
    REQUIRE_THROWS_AS(engine.modify(id, {101, 1, stamp(40)}), OrderNotActive);
    CHECK(state(engine, {1, 2}) == before);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Same-price reduction keeps FIFO and immutable creation metadata", "[modify][priority]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    MatchingEngine engine;
    const auto first = limit(engine, side, 100, 6).order;
    (void)limit(engine, side, 100, 5);
    const auto result = engine.modify(first.id, {std::nullopt, 3, stamp(30)});
    CHECK(result.trades.empty());
    CHECK(result.order.priority_sequence == first.priority_sequence);
    CHECK(result.order.creation_sequence == first.creation_sequence);
    CHECK(result.order.created_at == first.created_at);
    CHECK(result.order.updated_at == stamp(30));
    CHECK(result.order.original_quantity == 6);
    CHECK(result.order.quantity_adjustment == -3);
    const auto fill = market(engine, opposite(side), 4);
    REQUIRE(fill.trades.size() == 2);
    CHECK(fill.trades[0].maker_order_id == first.id);
    CHECK(fill.trades[0].quantity == 3);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Same-price increase loses FIFO and permits execution above original quantity", "[modify][priority]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    MatchingEngine engine;
    const auto first = limit(engine, side, 100, 2).order;
    const auto second = limit(engine, side, 100, 3).order;
    const auto modified = engine.modify(first.id, {std::nullopt, 5, stamp(30)});
    CHECK(modified.order.priority_sequence > second.priority_sequence);
    CHECK(modified.order.quantity_adjustment == 3);
    const auto fill = market(engine, opposite(side), 8);
    REQUIRE(fill.trades.size() == 2);
    CHECK(fill.trades[0].maker_order_id == second.id);
    CHECK(fill.trades[1].maker_order_id == first.id);
    CHECK(engine.find_order(first.id)->original_quantity == 2);
    CHECK(engine.find_order(first.id)->executed_quantity == 5);
    CHECK(engine.find_order(first.id)->status == OrderStatus::Filled);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Repricing joins the back of the destination queue", "[modify][priority]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    MatchingEngine engine;
    const auto first = limit(engine, side, side == Side::Buy ? 99 : 101, 4).order;
    const auto second = limit(engine, side, 100, 3).order;
    const auto result = engine.modify(first.id, {100, 2, stamp(30)});
    CHECK(result.order.id == first.id);
    CHECK(result.order.priority_sequence > second.priority_sequence);
    const auto fills = market(engine, opposite(side), 4);
    REQUIRE(fills.trades.size() == 2);
    CHECK(fills.trades[0].maker_order_id == second.id);
    CHECK(fills.trades[1].maker_order_id == first.id);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Crossing amendment sweeps multiple levels and retains its ID", "[modify][matching]") {
    const auto side = GENERATE(Side::Buy, Side::Sell);
    const auto amended_quantity = GENERATE(Quantity{6}, Quantity{12});
    MatchingEngine engine;
    const auto initial = limit(engine, side, side == Side::Buy ? 99 : 103, 8).order;
    const Price last = side == Side::Buy ? 102 : 100;
    (void)limit(engine, opposite(side), 101, 5);
    (void)limit(engine, opposite(side), last, 4);
    const auto result = engine.modify(initial.id, {last, amended_quantity, stamp(30)});
    CHECK(result.order.id == initial.id);
    CHECK(result.order.original_quantity == 8);
    CHECK(result.order.quantity_adjustment == amended_quantity - 8);
    REQUIRE(result.trades.size() == 2);
    CHECK(result.trades[0].quantity == 5);
    CHECK(result.trades[0].price == 101);
    CHECK(result.trades[1].quantity == (amended_quantity == 6 ? 1 : 4));
    CHECK(result.order.remaining_quantity == (amended_quantity == 6 ? 0 : 3));
    CHECK(result.order.status == (amended_quantity == 6 ? OrderStatus::Filled : OrderStatus::PartiallyFilled));
    CHECK(result.trades[0].execution_sequence > result.order.priority_sequence);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Repeated amendments and cancellation preserve cumulative accounting", "[modify][accounting]") {
    MatchingEngine engine;
    const auto original = limit(engine, Side::Buy, 100, 50).order;
    (void)market(engine, Side::Sell, 10);
    const auto reduced = engine.modify(original.id, {std::nullopt, 20, stamp(30)});
    CHECK(reduced.order.quantity_adjustment == -20);
    const auto raised = engine.modify(original.id, {101, 60, stamp(40)});
    CHECK(raised.order.quantity_adjustment == 20);
    (void)market(engine, Side::Sell, 5);
    const auto cancelled = engine.cancel(original.id, stamp(50));
    CHECK(cancelled.original_quantity == 50);
    CHECK(cancelled.executed_quantity == 15);
    CHECK(cancelled.remaining_quantity == 55);
    CHECK(cancelled.quantity_adjustment == 20);
    REQUIRE_NOTHROW(engine.check_invariants());
}

TEST_CASE("Identical amendments do not consume sequences or update timestamps", "[modify][noop]") {
    MatchingEngine engine;
    const auto original = limit(engine, Side::Buy, 100, 5).order;
    const auto mode = GENERATE(0, 1, 2);
    const ModifyOrder request{mode == 1 ? std::nullopt : std::optional<Price>{100},
        mode == 0 ? std::nullopt : std::optional<Quantity>{5}, stamp(999)};
    const auto result = engine.modify(original.id, request);
    CHECK(order_fields(result.order) == order_fields(original));
    CHECK(result.trades.empty());
    CHECK(limit(engine, Side::Buy, 99, 1).order.creation_sequence == 2);
    REQUIRE_NOTHROW(engine.check_invariants());
}
