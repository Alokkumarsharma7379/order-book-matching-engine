#pragma once

#include "orderbook/types.hpp"

#include <optional>

namespace orderbook {

// A value record. MatchingEngine will validate commands and own state transitions.
struct Order {
    OrderId id{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    std::optional<Price> price{};

    Quantity original_quantity{};
    Quantity remaining_quantity{};
    Quantity executed_quantity{};
    Quantity quantity_adjustment{};

    SequenceNumber creation_sequence{};
    SequenceNumber priority_sequence{};
    Timestamp created_at{};
    Timestamp updated_at{};
    OrderStatus status{OrderStatus::New};
};

} // namespace orderbook
