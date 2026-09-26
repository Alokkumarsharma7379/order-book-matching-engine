#pragma once

#include "orderbook/order_book.hpp"
#include "orderbook/trade.hpp"

#include <stdexcept>

namespace orderbook {

struct SubmitOrder {
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    std::optional<Price> price{};
    Quantity quantity{};
    Timestamp timestamp{};
    std::optional<OrderId> id{};
};

struct ExecutionResult {
    Order order;
    std::vector<Trade> trades;
};

struct ModifyOrder {
    std::optional<Price> price{};
    std::optional<Quantity> remaining_quantity{};
    Timestamp timestamp{};
};

class DuplicateOrderId : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

class UnknownOrderId : public std::out_of_range {
public:
    using std::out_of_range::out_of_range;
};

class OrderNotActive : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

class MatchingEngine {
public:
    [[nodiscard]] ExecutionResult submit(const SubmitOrder& request);
    [[nodiscard]] Order cancel(OrderId id, Timestamp timestamp);
    [[nodiscard]] ExecutionResult modify(OrderId id, const ModifyOrder& request);
    [[nodiscard]] std::optional<Order> find_order(OrderId id) const;
    [[nodiscard]] std::vector<PriceLevelSnapshot> bids() const;
    [[nodiscard]] std::vector<PriceLevelSnapshot> asks() const;
    [[nodiscard]] std::size_t active_order_count() const;
    [[nodiscard]] std::vector<Trade> trades(TradeId after_id = 0, std::size_t limit = 100) const;
    [[nodiscard]] bool healthy() const noexcept { return !failed_; }
    void check_invariants() const;

private:
    void ensure_healthy() const;
    void reserve_trades(std::size_t additional);
    Order& active_order(OrderId id);
    void plan_matches(ExecutionResult& result, SequenceNumber& sequence, TradeId& trade_id) const;
    void commit_trades(const ExecutionResult& result, SequenceNumber sequence, TradeId trade_id);

    OrderBook book_;
    std::vector<Trade> trades_;
    OrderId last_order_id_{};
    TradeId last_trade_id_{};
    SequenceNumber last_sequence_{};
    bool failed_{};
};

} // namespace orderbook
