#include "orderbook/matching_engine.hpp"

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
void rejects(Function action) {
    try {
        action();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error("expected rejection did not occur");
}

Side opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }

Order add(MatchingEngine& engine, Side side, Price price, Quantity quantity) {
    return engine.submit({side, OrderType::Limit, price, quantity, Timestamp{}, std::nullopt}).order;
}

ExecutionResult take(MatchingEngine& engine, Side side, Quantity quantity) {
    return engine.submit({side, OrderType::Market, std::nullopt, quantity, Timestamp{}, std::nullopt});
}

void check_cancellation(Side side) {
    MatchingEngine engine;
    const auto first = add(engine, side, 100, 5);
    const auto middle = add(engine, side, 100, 6);
    const auto last = add(engine, side, 100, 7);
    const Timestamp timestamp{std::chrono::microseconds{99}};
    const auto cancelled = engine.cancel(middle.id, timestamp);
    require(cancelled.status == OrderStatus::Cancelled && cancelled.remaining_quantity == 6
            && cancelled.updated_at == timestamp && cancelled.priority_sequence == middle.priority_sequence
            && engine.active_order_count() == 2 && engine.trades().empty(), "cancellation failed");
    const auto fill = take(engine, opposite(side), 12);
    require(fill.trades.size() == 2 && fill.trades[0].maker_order_id == first.id
            && fill.trades[1].maker_order_id == last.id, "cancel damaged FIFO");
    require(engine.bids().empty() && engine.asks().empty(), "empty level retained");
    rejects<OrderNotActive>([&] { (void)engine.cancel(middle.id, timestamp); });
    rejects<OrderNotActive>([&] { (void)engine.cancel(first.id, timestamp); });
    rejects<UnknownOrderId>([&] { (void)engine.cancel(999, timestamp); });
    rejects<std::invalid_argument>([&] { (void)engine.cancel(0, timestamp); });
    rejects<DuplicateOrderId>([&] {
        (void)engine.submit({side, OrderType::Limit, 100, 1, timestamp, middle.id});
    });
    const auto a = add(engine, side, 100, 1);
    const auto b = add(engine, side, 100, 2);
    const auto c = add(engine, side, 100, 3);
    (void)engine.cancel(a.id, timestamp);
    (void)engine.cancel(c.id, timestamp);
    require(take(engine, opposite(side), 1).trades[0].maker_order_id == b.id, "head/tail cancellation failed");
    const auto partial = engine.cancel(b.id, timestamp);
    require(partial.executed_quantity == 1 && partial.remaining_quantity == 1, "partial cancel lost history");
    engine.check_invariants();
}

void check_quantity_priority(Side side) {
    MatchingEngine engine;
    const auto first = add(engine, side, 100, 6);
    const auto second = add(engine, side, 100, 5);
    const auto reduced = engine.modify(first.id, {std::nullopt, 3, Timestamp{}});
    require(reduced.order.priority_sequence == first.priority_sequence
            && reduced.order.original_quantity == 6 && reduced.order.quantity_adjustment == -3
            && reduced.trades.empty(), "reduction changed priority or original quantity");
    const auto filled = take(engine, opposite(side), 4);
    require(filled.trades.size() == 2 && filled.trades[0].maker_order_id == first.id
            && filled.trades[0].quantity == 3 && filled.trades[1].maker_order_id == second.id,
            "reduction lost FIFO");
    (void)engine.cancel(second.id, Timestamp{});

    const auto earlier = add(engine, side, 100, 2);
    const auto later = add(engine, side, 100, 3);
    const auto increased = engine.modify(earlier.id, {std::nullopt, 5, Timestamp{}});
    require(increased.order.priority_sequence > later.priority_sequence
            && increased.order.creation_sequence == earlier.creation_sequence
            && increased.order.quantity_adjustment == 3, "increase did not lose priority");
    const auto next = take(engine, opposite(side), 4);
    require(next.trades.size() == 2 && next.trades[0].maker_order_id == later.id
            && next.trades[1].maker_order_id == earlier.id, "increased order jumped the queue");
    require(engine.find_order(earlier.id)->executed_quantity == 1, "wrong partial execution");
    (void)take(engine, opposite(side), 4);
    const auto history = engine.find_order(earlier.id).value();
    require(history.status == OrderStatus::Filled && history.executed_quantity == 5
            && history.original_quantity == 2 && history.quantity_adjustment == 3,
            "amended execution accounting failed");
    engine.check_invariants();
}

void check_repricing(Side side) {
    MatchingEngine engine;
    const auto first = add(engine, side, side == Side::Buy ? 99 : 101, 4);
    const auto second = add(engine, side, 100, 3);
    const auto changed = engine.modify(first.id, {100, std::nullopt, Timestamp{}});
    require(changed.order.id == first.id && changed.order.created_at == first.created_at
            && changed.order.priority_sequence > second.priority_sequence, "repricing metadata failed");
    const auto fills = take(engine, opposite(side), 4);
    require(fills.trades[0].maker_order_id == second.id && fills.trades[1].maker_order_id == first.id,
            "repriced order jumped target FIFO");
    engine.check_invariants();

    MatchingEngine crossing;
    const auto order = add(crossing, side, side == Side::Buy ? 99 : 103, 8);
    const Price best_price = 101;
    const Price next_price = side == Side::Buy ? 102 : 100;
    (void)add(crossing, opposite(side), best_price, 5);
    (void)add(crossing, opposite(side), next_price, 4);
    const auto matched = crossing.modify(order.id, {next_price, 6, Timestamp{}});
    require(matched.order.status == OrderStatus::Filled && matched.order.id == order.id
            && matched.order.original_quantity == 8 && matched.order.quantity_adjustment == -2
            && matched.trades.size() == 2 && matched.trades[0].price == best_price
            && matched.trades[0].quantity == 5 && matched.trades[1].quantity == 1,
            "crossing amendment failed");
    require(crossing.active_order_count() == 1, "filled amendment remained active");
    crossing.check_invariants();

    MatchingEngine remainder;
    const auto resting = add(remainder, side, side == Side::Buy ? 99 : 103, 5);
    (void)add(remainder, opposite(side), 101, 2);
    const auto partial = remainder.modify(resting.id, {101, std::nullopt, Timestamp{}});
    require(partial.order.remaining_quantity == 3 && partial.order.executed_quantity == 2
            && partial.order.status == OrderStatus::PartiallyFilled, "amended remainder did not rest");
    (void)remainder.modify(resting.id, {100, 1, Timestamp{}});
    const auto cancelled = remainder.cancel(resting.id, Timestamp{});
    require(cancelled.executed_quantity == 2 && cancelled.remaining_quantity == 1
            && cancelled.quantity_adjustment == -2, "partial amendment history failed");
    remainder.check_invariants();
}

void check_noop_and_errors() {
    MatchingEngine engine;
    const auto order = add(engine, Side::Buy, 100, 5);
    const Timestamp later{std::chrono::microseconds{99}};
    const auto same = engine.modify(order.id, {100, 5, later});
    require(same.order.updated_at == order.updated_at && same.order.priority_sequence == order.priority_sequence
            && same.trades.empty(), "no-op changed metadata");
    rejects<std::invalid_argument>([&] { (void)engine.modify(order.id, {}); });
    for (const Quantity quantity : {Quantity{0}, Quantity{-1}}) {
        rejects<std::invalid_argument>([&] { (void)engine.modify(order.id, {std::nullopt, quantity, later}); });
        rejects<std::invalid_argument>([&] { (void)engine.modify(order.id, {quantity, std::nullopt, later}); });
    }
    rejects<UnknownOrderId>([&] { (void)engine.modify(999, {101, std::nullopt, later}); });
    require(add(engine, Side::Buy, 99, 1).creation_sequence == 2, "rejection/no-op consumed sequence");
    (void)engine.cancel(order.id, later);
    rejects<OrderNotActive>([&] { (void)engine.modify(order.id, {101, std::nullopt, later}); });
    const auto expired = take(engine, Side::Buy, 1);
    rejects<OrderNotActive>([&] { (void)engine.modify(expired.order.id, {101, 1, later}); });
    (void)take(engine, Side::Sell, 1);
    rejects<OrderNotActive>([&] { (void)engine.modify(2, {101, 1, later}); });
    require(engine.healthy(), "expected rejection poisoned the engine");
    engine.check_invariants();
}

void check_overflow_rejection() {
    constexpr Quantity maximum = std::numeric_limits<Quantity>::max();
    MatchingEngine engine;
    const auto first = add(engine, Side::Buy, 100, maximum - 1);
    (void)add(engine, Side::Buy, 100, 1);
    rejects<std::overflow_error>([&] { (void)engine.modify(first.id, {std::nullopt, maximum, Timestamp{}}); });
    require(engine.find_order(first.id)->priority_sequence == first.priority_sequence
            && engine.bids()[0].total_quantity == maximum, "failed increase changed the book");
    const auto third = add(engine, Side::Buy, 99, 1);
    require(third.creation_sequence == 3, "failed increase consumed a sequence");
    rejects<std::overflow_error>([&] { (void)engine.modify(third.id, {100, std::nullopt, Timestamp{}}); });
    require(engine.find_order(third.id)->price == 99 && engine.bids().size() == 2,
            "failed reprice detached the order");
    (void)take(engine, Side::Sell, 1);
    rejects<std::overflow_error>([&] { (void)engine.modify(first.id, {std::nullopt, maximum, Timestamp{}}); });
    require(engine.healthy(), "overflow poisoned the engine");
    engine.check_invariants();
}

void check_repeated_relocation() {
    MatchingEngine engine;
    for (OrderId id = 1; id <= 256; ++id) {
        (void)add(engine, Side::Buy, 100 + id % 16, 2);
    }
    for (OrderId id = 1; id <= 256; ++id) {
        (void)engine.modify(id, {std::nullopt, 3, Timestamp{}});
        (void)engine.modify(id, {200 + id % 8, std::nullopt, Timestamp{}});
        if (id % 2 != 0) {
            (void)engine.cancel(id, Timestamp{});
        }
        engine.check_invariants();
    }
    require(take(engine, Side::Sell, 384).order.status == OrderStatus::Filled,
            "relocation/cancellation lost liquidity");
    require(engine.active_order_count() == 0, "relocation left stale locations");
    engine.check_invariants();
}

} // namespace

int main() {
    try {
        for (const auto side : {Side::Buy, Side::Sell}) {
            check_cancellation(side);
            check_quantity_priority(side);
            check_repricing(side);
        }
        check_noop_and_errors();
        check_overflow_rejection();
        check_repeated_relocation();
        std::cout << "Cancellation and modification checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Order management check failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
