#pragma once

#include "orderbook/types.hpp"

namespace orderbook {

// Execution records are immutable by engine convention once emitted.
struct Trade {
    TradeId id{};
    OrderId maker_order_id{};
    OrderId taker_order_id{};
    Price price{};
    Quantity quantity{};
    SequenceNumber execution_sequence{};
    Timestamp executed_at{};
};

} // namespace orderbook
