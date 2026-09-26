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

ExecutionResult limit(MatchingEngine& engine, Side side, Price price, Quantity quantity,
                      std::optional<OrderId> id = std::nullopt) {
    return engine.submit({side, OrderType::Limit, price, quantity, Timestamp{}, id});
}

ExecutionResult market(MatchingEngine& engine, Side side, Quantity quantity) {
    return engine.submit({side, OrderType::Market, std::nullopt, quantity, Timestamp{}, std::nullopt});
}

void check_sweep() {
    MatchingEngine engine;
    const auto first = limit(engine, Side::Sell, 9'900, 20).order.id;
    const auto second = limit(engine, Side::Sell, 10'000, 30).order.id;
    const auto third = limit(engine, Side::Sell, 10'100, 70).order.id;
    const auto result = limit(engine, Side::Buy, 10'100, 100);
    require(result.order.status == OrderStatus::Filled && result.order.executed_quantity == 100
            && result.order.remaining_quantity == 0 && result.trades.size() == 3, "sweep failed");
    require(result.trades[0].maker_order_id == first && result.trades[0].price == 9'900
            && result.trades[0].quantity == 20, "wrong first fill");
    require(result.trades[1].maker_order_id == second && result.trades[1].price == 10'000
            && result.trades[1].quantity == 30, "wrong second fill");
    require(result.trades[2].maker_order_id == third && result.trades[2].price == 10'100
            && result.trades[2].quantity == 50, "wrong third fill");
    require(engine.find_order(first)->status == OrderStatus::Filled, "filled maker history missing");
    require(engine.find_order(third)->remaining_quantity == 20 && engine.active_order_count() == 1,
            "wrong maker remainder");
    require(engine.asks()[0].total_quantity == 20 && engine.bids().empty(), "wrong sweep book");
    require(engine.trades(1, 1).size() == 1 && engine.trades(1, 1)[0].id == 2,
            "trade pagination failed");
    require(engine.trades(3).empty() && engine.trades(0, 0).empty(), "trade boundary query failed");
    rejects<std::invalid_argument>([&] { (void)engine.trades(-1); });
    auto snapshot = engine.trades();
    snapshot[0].quantity = 1;
    require(engine.trades()[0].quantity == 20, "trade snapshot changed history");
    engine.check_invariants();
}

void check_sell_and_fifo() {
    MatchingEngine engine;
    const auto best = limit(engine, Side::Buy, 10'100, 8).order.id;
    const auto earlier = limit(engine, Side::Buy, 10'000, 7).order.id;
    const auto later = limit(engine, Side::Buy, 10'000, 9).order.id;
    const auto sell = limit(engine, Side::Sell, 10'000, 12);
    require(sell.trades.size() == 2 && sell.trades[0].maker_order_id == best
            && sell.trades[0].price == 10'100 && sell.trades[0].quantity == 8
            && sell.trades[1].maker_order_id == earlier && sell.trades[1].quantity == 4,
            "sell price priority failed");
    const auto next = market(engine, Side::Sell, 6);
    require(next.trades.size() == 2 && next.trades[0].maker_order_id == earlier
            && next.trades[0].quantity == 3 && next.trades[1].maker_order_id == later
            && next.trades[1].quantity == 3, "partial maker lost FIFO");
    const auto exhausted = market(engine, Side::Sell, 100);
    require(exhausted.order.status == OrderStatus::Cancelled
            && exhausted.order.executed_quantity == 6 && exhausted.order.remaining_quantity == 94
            && engine.active_order_count() == 0 && engine.bids().empty(), "market remainder rested");
    for (const auto side : {Side::Buy, Side::Sell}) {
        const auto empty = market(engine, side, 5);
        require(empty.trades.empty() && empty.order.status == OrderStatus::Cancelled
                && empty.order.remaining_quantity == 5, "empty market order failed");
    }
    engine.check_invariants();
}

void check_limit_remainders() {
    for (const auto incoming_side : {Side::Buy, Side::Sell}) {
        MatchingEngine engine;
        const auto opposite = incoming_side == Side::Buy ? Side::Sell : Side::Buy;
        const auto maker = limit(engine, opposite, 100, 5).order.id;
        const auto result = limit(engine, incoming_side, 100, 8);
        require(result.order.status == OrderStatus::PartiallyFilled
                && result.order.remaining_quantity == 3 && result.order.executed_quantity == 5
                && engine.active_order_count() == 1, "limit remainder did not rest");
        require(engine.find_order(maker)->status == OrderStatus::Filled, "maker was not filled");
        const auto finish = market(engine, opposite, 5);
        require(finish.order.status == OrderStatus::Cancelled
                && finish.order.executed_quantity == 3 && finish.order.remaining_quantity == 2
                && finish.trades[0].maker_order_id == result.order.id, "rested remainder did not match");
        require(engine.bids().empty() && engine.asks().empty(), "empty levels were retained");
        engine.check_invariants();
    }
    MatchingEngine engine;
    (void)limit(engine, Side::Sell, 102, 5);
    const auto noncrossing = limit(engine, Side::Buy, 101, 5);
    require(noncrossing.trades.empty() && noncrossing.order.status == OrderStatus::New,
            "noncrossing limit executed");
    engine.check_invariants();
}

void check_ask_fifo_and_limit_boundary() {
    MatchingEngine engine;
    const auto first = engine.submit({Side::Sell, OrderType::Limit, 100, 2,
        Timestamp{std::chrono::microseconds{10}}, std::nullopt}).order.id;
    const auto second = limit(engine, Side::Sell, 100, 3).order.id;
    (void)limit(engine, Side::Sell, 101, 4);
    const auto buy = limit(engine, Side::Buy, 100, 4);
    require(buy.trades.size() == 2 && buy.trades[0].maker_order_id == first
            && buy.trades[0].quantity == 2 && buy.trades[1].maker_order_id == second
            && buy.trades[1].quantity == 2, "ask FIFO followed timestamps instead of sequence");
    const auto remainder = limit(engine, Side::Buy, 100, 3);
    require(remainder.trades.size() == 1 && remainder.trades[0].quantity == 1
            && remainder.order.remaining_quantity == 2 && engine.asks()[0].price == 101
            && engine.asks()[0].total_quantity == 4 && engine.bids()[0].price == 100,
            "limit order consumed an unacceptable price");
    engine.check_invariants();
}

void check_rejections_and_limits() {
    MatchingEngine engine;
    SubmitOrder request{Side::Buy, OrderType::Limit, 100, 1, Timestamp{}, std::nullopt};
    for (const Quantity quantity : {Quantity{0}, Quantity{-1}}) {
        auto invalid = request;
        invalid.quantity = quantity;
        rejects<std::invalid_argument>([&] { (void)engine.submit(invalid); });
    }
    for (const auto price : {std::optional<Price>{}, std::optional<Price>{0}, std::optional<Price>{-1}}) {
        auto invalid = request;
        invalid.price = price;
        rejects<std::invalid_argument>([&] { (void)engine.submit(invalid); });
    }
    auto invalid = request;
    invalid.type = OrderType::Market;
    rejects<std::invalid_argument>([&] { (void)engine.submit(invalid); });
    invalid = request;
    invalid.side = static_cast<Side>(99);
    rejects<std::invalid_argument>([&] { (void)engine.submit(invalid); });
    invalid = request;
    invalid.type = static_cast<OrderType>(99);
    rejects<std::invalid_argument>([&] { (void)engine.submit(invalid); });
    for (const OrderId id : {OrderId{0}, OrderId{-1}}) {
        invalid = request;
        invalid.id = id;
        rejects<std::invalid_argument>([&] { (void)engine.submit(invalid); });
    }
    require(engine.active_order_count() == 0 && engine.trades().empty() && engine.healthy(),
            "rejection changed engine state");
    const auto accepted = engine.submit(request);
    require(accepted.order.id == 1 && accepted.order.creation_sequence == 1, "rejection consumed counters");
    (void)market(engine, Side::Sell, 1);
    rejects<DuplicateOrderId>([&] { (void)limit(engine, Side::Buy, 100, 1, 1); });
    require(limit(engine, Side::Buy, 100, 1).order.id == 3, "duplicate consumed an ID");
    engine.check_invariants();

    MatchingEngine large;
    constexpr auto maximum = std::numeric_limits<Quantity>::max();
    (void)limit(large, Side::Sell, 100, maximum);
    rejects<std::overflow_error>([&] { (void)limit(large, Side::Sell, 100, 1); });
    require(large.healthy() && large.active_order_count() == 1, "overflow changed book");
    const auto fill = market(large, Side::Buy, maximum);
    require(fill.order.id == 2 && fill.order.status == OrderStatus::Filled
            && fill.trades[0].quantity == maximum, "large quantity execution failed");
    large.check_invariants();

    MatchingEngine ids;
    (void)limit(ids, Side::Buy, 100, 1, std::numeric_limits<OrderId>::max());
    rejects<std::overflow_error>([&] { (void)limit(ids, Side::Buy, 99, 1); });
    require(ids.healthy() && ids.active_order_count() == 1, "ID exhaustion changed book");
    ids.check_invariants();
}

void check_repeated_sweeps() {
    MatchingEngine engine;
    for (int batch = 0; batch < 100; ++batch) {
        for (Price price = 90; price < 100; ++price) {
            (void)limit(engine, Side::Buy, price, 1);
        }
        require(market(engine, Side::Sell, 10).trades.size() == 10, "bid sweep failed");
        for (Price price = 110; price < 120; ++price) {
            (void)limit(engine, Side::Sell, price, 1);
        }
        require(market(engine, Side::Buy, 10).trades.size() == 10, "ask sweep failed");
        require(engine.active_order_count() == 0, "sweep left active orders");
        engine.check_invariants();
    }
    require(engine.trades(0, 3000).size() == 2000, "trade history lost executions");
}

} // namespace

int main() {
    try {
        check_sweep();
        check_sell_and_fifo();
        check_limit_remainders();
        check_ask_fifo_and_limit_boundary();
        check_rejections_and_limits();
        check_repeated_sweeps();
        std::cout << "Matching checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Matching check failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
