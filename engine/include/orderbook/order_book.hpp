#pragma once

#include "orderbook/order.hpp"
#include "orderbook/price_level.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace orderbook {

class OrderBook {
public:
    OrderBook() = default;
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    // Storage operation: accepts only valid, non-crossing resting limit orders.
    void add_resting(const Order& order);

    [[nodiscard]] std::optional<Order> find_order(OrderId id) const;
    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::size_t active_order_count() const noexcept;
    [[nodiscard]] std::vector<PriceLevelSnapshot> bids() const;
    [[nodiscard]] std::vector<PriceLevelSnapshot> asks() const;
    [[nodiscard]] std::vector<Order> orders_at(Side side, Price price) const;

    // Explicit diagnostic; not called on every hot-path operation.
    void check_invariants() const;

private:
    friend class MatchingEngine;
    void add_resting_impl(const Order& order, bool require_uncrossed);
    void remove_active(OrderId id);
    void replace_active(const Order& replacement, bool retain_priority);

    struct Location {
        Side side;
        Price price;
        std::list<OrderId>::iterator position;
    };

    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel> asks_;
    std::unordered_map<OrderId, Order> orders_;
    std::unordered_map<OrderId, Location> locations_;
};

} // namespace orderbook
