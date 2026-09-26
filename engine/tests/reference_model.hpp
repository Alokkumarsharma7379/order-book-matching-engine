#pragma once

#include "orderbook/matching_engine.hpp"

#include <algorithm>
#include <stdexcept>

namespace test_support {

// Deliberately slow oracle for bounded, valid commands. No book indexes or FIFO lists.
class ReferenceModel {
public:
    orderbook::ExecutionResult submit(const orderbook::SubmitOrder& request) {
        using namespace orderbook;
        const OrderId id = request.id ? *request.id : largest_id_ + 1;
        largest_id_ = std::max(largest_id_, id);
        const auto arrival = ++sequence_;
        records_.push_back(Order{
            .id = id, .side = request.side, .type = request.type, .price = request.price,
            .original_quantity = request.quantity, .remaining_quantity = request.quantity,
            .executed_quantity = 0, .quantity_adjustment = 0,
            .creation_sequence = arrival, .priority_sequence = arrival,
            .created_at = request.timestamp, .updated_at = request.timestamp,
            .status = OrderStatus::New});
        return match(records_.back());
    }

    orderbook::Order cancel(orderbook::OrderId id, orderbook::Timestamp timestamp) {
        auto& order = get(id);
        ++sequence_;
        order.status = orderbook::OrderStatus::Cancelled;
        order.updated_at = timestamp;
        return order;
    }

    orderbook::ExecutionResult modify(orderbook::OrderId id, const orderbook::ModifyOrder& request) {
        auto& order = get(id);
        const auto price = request.price.value_or(*order.price);
        const auto quantity = request.remaining_quantity.value_or(order.remaining_quantity);
        if (price == order.price && quantity == order.remaining_quantity) {
            return {order, {}};
        }
        const bool retains = price == order.price && quantity < order.remaining_quantity;
        ++sequence_;
        order.quantity_adjustment += quantity - order.remaining_quantity;
        order.remaining_quantity = quantity;
        order.price = price;
        order.updated_at = request.timestamp;
        if (retains) {
            return {order, {}};
        }
        order.priority_sequence = sequence_;
        return match(order);
    }

    [[nodiscard]] std::vector<orderbook::OrderId> active_ids() const {
        std::vector<orderbook::OrderId> ids;
        for (const auto& order : records_) {
            if (active(order)) {
                ids.push_back(order.id);
            }
        }
        return ids;
    }

    [[nodiscard]] std::vector<orderbook::PriceLevelSnapshot> levels(orderbook::Side side) const {
        std::vector<orderbook::PriceLevelSnapshot> result;
        for (const auto& order : records_) {
            if (!active(order) || order.side != side) {
                continue;
            }
            const auto found = std::find_if(result.begin(), result.end(),
                [&](const auto& level) { return level.price == order.price; });
            if (found == result.end()) {
                result.push_back({*order.price, order.remaining_quantity, 1});
            } else {
                found->total_quantity += order.remaining_quantity;
                ++found->order_count;
            }
        }
        std::sort(result.begin(), result.end(), [&](const auto& left, const auto& right) {
            return side == orderbook::Side::Buy ? left.price > right.price : left.price < right.price;
        });
        return result;
    }

    [[nodiscard]] const auto& records() const noexcept { return records_; }
    [[nodiscard]] const auto& trades() const noexcept { return trades_; }

private:
    static bool active(const orderbook::Order& order) {
        using namespace orderbook;
        return order.type == OrderType::Limit && order.remaining_quantity > 0
            && (order.status == OrderStatus::New || order.status == OrderStatus::PartiallyFilled);
    }

    orderbook::Order& get(orderbook::OrderId id) {
        const auto found = std::find_if(records_.begin(), records_.end(),
            [&](const auto& order) { return order.id == id; });
        if (found == records_.end()) {
            throw std::logic_error("reference model received an unknown ID");
        }
        return *found;
    }

    orderbook::Order* best_maker(const orderbook::Order& taker) {
        using namespace orderbook;
        Order* best = nullptr;
        for (auto& candidate : records_) {
            if (!active(candidate) || candidate.side == taker.side) {
                continue;
            }
            if (taker.price && (taker.side == Side::Buy ? candidate.price > taker.price
                                                       : candidate.price < taker.price)) {
                continue;
            }
            if (best == nullptr
                || (taker.side == Side::Buy ? candidate.price < best->price : candidate.price > best->price)
                || (candidate.price == best->price && candidate.priority_sequence < best->priority_sequence)) {
                best = &candidate;
            }
        }
        return best;
    }

    orderbook::ExecutionResult match(orderbook::Order& taker) {
        using namespace orderbook;
        std::vector<Trade> fills;
        while (taker.remaining_quantity > 0) {
            auto* maker = best_maker(taker);
            if (maker == nullptr) {
                break;
            }
            const Quantity quantity = std::min(maker->remaining_quantity, taker.remaining_quantity);
            const Trade trade{++trade_id_, maker->id, taker.id, *maker->price,
                              quantity, ++sequence_, taker.updated_at};
            maker->remaining_quantity -= quantity;
            maker->executed_quantity += quantity;
            maker->updated_at = taker.updated_at;
            maker->status = maker->remaining_quantity == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
            taker.remaining_quantity -= quantity;
            taker.executed_quantity += quantity;
            fills.push_back(trade);
            trades_.push_back(trade);
        }
        taker.status = taker.remaining_quantity == 0 ? OrderStatus::Filled
            : taker.type == OrderType::Market ? OrderStatus::Cancelled
            : taker.executed_quantity > 0 ? OrderStatus::PartiallyFilled : OrderStatus::New;
        return {taker, fills};
    }

    std::vector<orderbook::Order> records_;
    std::vector<orderbook::Trade> trades_;
    orderbook::OrderId largest_id_{};
    orderbook::TradeId trade_id_{};
    orderbook::SequenceNumber sequence_{};
};

} // namespace test_support
