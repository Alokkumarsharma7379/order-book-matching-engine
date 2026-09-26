#include "orderbook/order_book.hpp"

#include <iterator>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

namespace orderbook {
namespace {

static_assert(std::is_nothrow_copy_assignable_v<Order>);
static_assert(std::is_nothrow_copy_constructible_v<Order>);

bool valid_record(const Order& order) noexcept {
    return order.id > 0 && (order.side == Side::Buy || order.side == Side::Sell)
        && ((order.type == OrderType::Limit && order.price && *order.price > 0)
            || (order.type == OrderType::Market && !order.price))
        && order.original_quantity > 0 && order.remaining_quantity >= 0
        && order.executed_quantity >= 0
        && order.executed_quantity <=
            std::numeric_limits<Quantity>::max() - order.remaining_quantity
        && order.quantity_adjustment ==
            order.executed_quantity + order.remaining_quantity - order.original_quantity
        && order.creation_sequence > 0
        && order.priority_sequence >= order.creation_sequence;
}

bool valid_resting_record(const Order& order) noexcept {
    return valid_record(order) && order.type == OrderType::Limit
        && order.remaining_quantity > 0
        && ((order.status == OrderStatus::New && order.executed_quantity == 0)
            || (order.status == OrderStatus::PartiallyFilled
                && order.executed_quantity > 0));
}

template <typename Levels>
std::vector<PriceLevelSnapshot> snapshots(const Levels& levels) {
    std::vector<PriceLevelSnapshot> result;
    result.reserve(levels.size());
    for (const auto& [price, level] : levels) {
        result.push_back({price, level.total_quantity, level.orders.size()});
    }
    return result;
}

} // namespace

void OrderBook::add_resting(const Order& order) {
    add_resting_impl(order, true);
}

void OrderBook::add_resting_impl(const Order& order, bool require_uncrossed) {
    if (!valid_resting_record(order)) {
        throw std::invalid_argument("invalid resting limit order");
    }
    if (orders_.contains(order.id)) {
        throw std::invalid_argument("duplicate order ID");
    }
    const auto opposite = order.side == Side::Buy ? best_ask() : best_bid();
    if (require_uncrossed && opposite
        && (order.side == Side::Buy ? *order.price >= *opposite
                                   : *order.price <= *opposite)) {
        throw std::invalid_argument("crossing order must be matched before resting");
    }

    const auto insert = [&](auto& levels) {
        const auto [level_it, created] = levels.try_emplace(*order.price);
        auto& level = level_it->second;
        try {
            if (order.remaining_quantity >
                std::numeric_limits<Quantity>::max() - level.total_quantity) {
                throw std::overflow_error("price-level quantity exceeds int64 range");
            }
            if (!level.orders.empty()
                && orders_.at(level.orders.back()).priority_sequence >= order.priority_sequence) {
                throw std::invalid_argument("priority sequence must increase within a level");
            }

            const auto record_it = orders_.emplace(order.id, order).first;
            try {
                level.orders.push_back(order.id);
                try {
                    locations_.emplace(order.id, Location{
                        order.side, *order.price, std::prev(level.orders.end())});
                } catch (...) {
                    level.orders.pop_back();
                    throw;
                }
            } catch (...) {
                orders_.erase(record_it);
                throw;
            }
            level.total_quantity += order.remaining_quantity;
        } catch (...) {
            // Roll back a newly allocated level as well as any inserted record/node.
            if (created) {
                levels.erase(level_it);
            }
            throw;
        }
    };

    if (order.side == Side::Buy) {
        insert(bids_);
    } else {
        insert(asks_);
    }
}

void OrderBook::remove_active(OrderId id) {
    const auto location_it = locations_.find(id);
    if (location_it == locations_.end()) {
        throw std::logic_error("active location missing");
    }
    const auto location = location_it->second;
    const auto quantity = orders_.at(id).remaining_quantity;
    const auto remove = [&](auto& levels) {
        const auto level_it = levels.find(location.price);
        if (level_it == levels.end()) {
            throw std::logic_error("active price level missing");
        }
        auto& level = level_it->second;
        level.total_quantity -= quantity;
        level.orders.erase(location.position);
        locations_.erase(location_it);
        if (level.orders.empty()) {
            levels.erase(level_it);
        }
    };
    if (location.side == Side::Buy) {
        remove(bids_);
    } else {
        remove(asks_);
    }
}

void OrderBook::replace_active(const Order& replacement, bool retain_priority) {
    auto& previous = orders_.at(replacement.id);
    auto location_it = locations_.find(replacement.id);
    if (location_it == locations_.end() || !valid_record(replacement)
        || replacement.side != previous.side || replacement.type != OrderType::Limit
        || replacement.original_quantity != previous.original_quantity
        || replacement.creation_sequence != previous.creation_sequence
        || replacement.created_at != previous.created_at
        || replacement.executed_quantity < previous.executed_quantity) {
        throw std::logic_error("invalid active-order replacement");
    }
    if (replacement.remaining_quantity == 0) {
        if (retain_priority || replacement.status != OrderStatus::Filled) {
            throw std::logic_error("invalid terminal replacement");
        }
        remove_active(replacement.id);
        previous = replacement;
        return;
    }
    if (!valid_resting_record(replacement)
        || (retain_priority && (replacement.price != previous.price
            || replacement.remaining_quantity > previous.remaining_quantity
            || replacement.priority_sequence != previous.priority_sequence))) {
        throw std::logic_error("invalid resting replacement");
    }

    const auto relocate = [&](auto& levels) {
        const auto source = levels.find(location_it->second.price);
        if (source == levels.end()) {
            throw std::logic_error("source price level missing");
        }
        const auto [target, created] = levels.try_emplace(*replacement.price);
        try {
            const auto base = target->second.total_quantity
                - (source == target ? previous.remaining_quantity : 0);
            if (replacement.remaining_quantity > std::numeric_limits<Quantity>::max() - base) {
                throw std::overflow_error("price-level quantity exceeds int64 range");
            }
            if (!retain_priority && !target->second.orders.empty()
                && orders_.at(target->second.orders.back()).priority_sequence
                    >= replacement.priority_sequence) {
                throw std::logic_error("replacement must receive later time priority");
            }
        } catch (...) {
            if (created) {
                levels.erase(target);
            }
            throw;
        }

        // All allocation and validation precede this non-allocating relocation.
        source->second.total_quantity -= previous.remaining_quantity;
        target->second.total_quantity += replacement.remaining_quantity;
        if (!retain_priority) {
            target->second.orders.splice(target->second.orders.end(), source->second.orders,
                                         location_it->second.position);
        }
        location_it->second.price = *replacement.price;
        previous = replacement;
        if (source != target && source->second.orders.empty()) {
            levels.erase(source);
        }
    };
    if (previous.side == Side::Buy) {
        relocate(bids_);
    } else {
        relocate(asks_);
    }
}

std::optional<Order> OrderBook::find_order(OrderId id) const {
    const auto it = orders_.find(id);
    return it == orders_.end() ? std::nullopt : std::optional<Order>{it->second};
}

std::optional<Price> OrderBook::best_bid() const noexcept {
    return bids_.empty() ? std::nullopt : std::optional<Price>{bids_.begin()->first};
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    return asks_.empty() ? std::nullopt : std::optional<Price>{asks_.begin()->first};
}

std::size_t OrderBook::active_order_count() const noexcept {
    return locations_.size();
}

std::vector<PriceLevelSnapshot> OrderBook::bids() const {
    return snapshots(bids_);
}

std::vector<PriceLevelSnapshot> OrderBook::asks() const {
    return snapshots(asks_);
}

std::vector<Order> OrderBook::orders_at(Side side, Price price) const {
    if ((side != Side::Buy && side != Side::Sell) || price <= 0) {
        throw std::invalid_argument("invalid side or price");
    }
    const auto collect = [&](const auto& levels) {
        std::vector<Order> result;
        if (const auto it = levels.find(price); it != levels.end()) {
            result.reserve(it->second.orders.size());
            for (const auto id : it->second.orders) {
                result.push_back(orders_.at(id));
            }
        }
        return result;
    };
    return side == Side::Buy ? collect(bids_) : collect(asks_);
}

void OrderBook::check_invariants() const {
    std::unordered_set<OrderId> seen;
    const auto check = [&](const auto& levels, Side side) {
        for (const auto& [price, level] : levels) {
            if (price <= 0 || level.orders.empty()) {
                throw std::logic_error("invalid or empty price level");
            }
            Quantity total = 0;
            SequenceNumber previous = 0;
            for (auto it = level.orders.cbegin(); it != level.orders.cend(); ++it) {
                const auto record = orders_.find(*it);
                const auto location = locations_.find(*it);
                if (record == orders_.end() || location == locations_.end()
                    || !seen.insert(*it).second) {
                    throw std::logic_error("missing or repeated active order");
                }
                const auto& order = record->second;
                const auto& stored = location->second;
                if (order.id != *it || !valid_resting_record(order)
                    || order.side != side || *order.price != price
                    || stored.side != side || stored.price != price
                    || stored.position != it || order.priority_sequence <= previous) {
                    throw std::logic_error("order, location, or FIFO mismatch");
                }
                if (order.remaining_quantity > std::numeric_limits<Quantity>::max() - total) {
                    throw std::logic_error("price-level quantity overflow");
                }
                total += order.remaining_quantity;
                previous = order.priority_sequence;
            }
            if (total != level.total_quantity) {
                throw std::logic_error("cached price-level quantity mismatch");
            }
        }
    };
    check(bids_, Side::Buy);
    check(asks_, Side::Sell);
    if (seen.size() != locations_.size()) {
        throw std::logic_error("unindexed location");
    }
    for (const auto& [id, order] : orders_) {
        if (seen.contains(id)) {
            continue;
        }
        const bool terminal =
            (order.status == OrderStatus::Filled && order.remaining_quantity == 0
                && order.executed_quantity > 0)
            || (order.status == OrderStatus::Cancelled && order.remaining_quantity > 0);
        if (id != order.id || !valid_record(order) || !terminal) {
            throw std::logic_error("invalid inactive order record");
        }
    }
    if (best_bid() && best_ask() && *best_bid() >= *best_ask()) {
        throw std::logic_error("crossed book");
    }
}

} // namespace orderbook
