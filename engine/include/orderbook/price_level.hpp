#pragma once

#include "orderbook/types.hpp"

#include <cstddef>
#include <list>

namespace orderbook {

struct PriceLevel {
    std::list<OrderId> orders;
    Quantity total_quantity{};
};

struct PriceLevelSnapshot {
    Price price{};
    Quantity total_quantity{};
    std::size_t order_count{};
};

} // namespace orderbook
