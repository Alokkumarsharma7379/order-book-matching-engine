#pragma once

#include "orderbook/matching_engine.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <limits>
#include <tuple>

namespace test_support {
using namespace orderbook;

inline Timestamp stamp(std::int64_t value) {
    return Timestamp{std::chrono::microseconds{value}};
}

inline Side opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }

inline SubmitOrder limit_request(Side side, Price price, Quantity quantity,
                                 std::optional<OrderId> id = std::nullopt) {
    return {side, OrderType::Limit, price, quantity, stamp(10), id};
}

inline ExecutionResult limit(MatchingEngine& engine, Side side, Price price, Quantity quantity) {
    return engine.submit(limit_request(side, price, quantity));
}

inline ExecutionResult market(MatchingEngine& engine, Side side, Quantity quantity) {
    return engine.submit({side, OrderType::Market, std::nullopt, quantity, stamp(20), std::nullopt});
}

inline auto order_fields(const Order& order) {
    return std::tuple{order.id, order.side, order.type, order.price, order.original_quantity,
        order.remaining_quantity, order.executed_quantity, order.quantity_adjustment,
        order.creation_sequence, order.priority_sequence, order.created_at,
        order.updated_at, order.status};
}

inline auto trade_fields(const Trade& trade) {
    return std::tuple{trade.id, trade.maker_order_id, trade.taker_order_id,
        trade.price, trade.quantity, trade.execution_sequence, trade.executed_at};
}

inline auto level_fields(const std::vector<PriceLevelSnapshot>& levels) {
    std::vector<std::tuple<Price, Quantity, std::size_t>> result;
    for (const auto& level : levels) {
        result.emplace_back(level.price, level.total_quantity, level.order_count);
    }
    return result;
}

inline void same_result(const ExecutionResult& actual, const ExecutionResult& expected) {
    CHECK(order_fields(actual.order) == order_fields(expected.order));
    REQUIRE(actual.trades.size() == expected.trades.size());
    for (std::size_t i = 0; i < actual.trades.size(); ++i) {
        CHECK(trade_fields(actual.trades[i]) == trade_fields(expected.trades[i]));
    }
}

inline auto state(const MatchingEngine& engine, std::initializer_list<OrderId> ids) {
    using OrderFields = decltype(order_fields(Order{}));
    using TradeFields = decltype(trade_fields(Trade{}));
    std::vector<std::optional<OrderFields>> orders;
    for (const auto id : ids) {
        const auto order = engine.find_order(id);
        orders.push_back(order ? std::optional<OrderFields>{order_fields(*order)} : std::nullopt);
    }
    std::vector<TradeFields> trades;
    for (const auto& trade : engine.trades(0, std::numeric_limits<std::size_t>::max())) {
        trades.push_back(trade_fields(trade));
    }
    return std::tuple{orders, trades, level_fields(engine.bids()), level_fields(engine.asks()),
                      engine.active_order_count(), engine.healthy()};
}

inline Order resting(OrderId id, Side side, Price price, Quantity quantity, SequenceNumber sequence) {
    return {.id = id, .side = side, .type = OrderType::Limit, .price = price,
        .original_quantity = quantity, .remaining_quantity = quantity, .executed_quantity = 0,
        .quantity_adjustment = 0, .creation_sequence = sequence, .priority_sequence = sequence,
        .created_at = stamp(sequence), .updated_at = stamp(sequence), .status = OrderStatus::New};
}

} // namespace test_support
