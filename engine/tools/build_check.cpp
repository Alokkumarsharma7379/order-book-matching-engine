#include "orderbook/order.hpp"
#include "orderbook/trade.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <span>

constexpr int sum(std::span<const int> values) {
    int result = 0;
    for (const int value : values) {
        result += value;
    }
    return result;
}

int main() {
    std::array values{3, 1, 2};
    std::ranges::sort(values);

    if (values != std::array{1, 2, 3} || sum(values) != 6) {
        std::cerr << "C++20 build check failed\n";
        return 1;
    }

    // Hand-constructed records verify the public headers compile; no matching runs.
    const orderbook::Timestamp created_at{std::chrono::microseconds{1'000'000}};
    const auto executed_at = created_at + std::chrono::microseconds{1};
    const orderbook::Order sample_order{
        .id = 1,
        .side = orderbook::Side::Sell,
        .type = orderbook::OrderType::Limit,
        .price = 10'100,
        .original_quantity = 50,
        .remaining_quantity = 40,
        .executed_quantity = 10,
        .creation_sequence = 1,
        .priority_sequence = 1,
        .created_at = created_at,
        .updated_at = executed_at,
        .status = orderbook::OrderStatus::PartiallyFilled
    };
    const orderbook::Trade sample_trade{
        .id = 1,
        .maker_order_id = sample_order.id,
        .taker_order_id = 2,
        .price = sample_order.price.value(),
        .quantity = 10,
        .execution_sequence = 3,
        .executed_at = executed_at
    };

    std::cout << "C++20 build check passed\n";
    std::cout << "Domain models compiled (sample IDs: order " << sample_order.id
              << ", trade " << sample_trade.id << ")\n";
    return 0;
}
