#pragma once

#include <chrono>
#include <cstdint>

namespace orderbook {

using Price = std::int64_t;
using Quantity = std::int64_t;
using OrderId = std::int64_t;
using TradeId = std::int64_t;
using SequenceNumber = std::int64_t;
using Timestamp = std::chrono::sys_time<std::chrono::microseconds>;

enum class Side {
    Buy,
    Sell
};

enum class OrderType {
    Limit,
    Market
};

enum class OrderStatus {
    New,
    PartiallyFilled,
    Filled,
    Cancelled
};

} // namespace orderbook
