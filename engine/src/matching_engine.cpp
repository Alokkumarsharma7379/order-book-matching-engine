#include "orderbook/matching_engine.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace orderbook {
namespace {

std::int64_t next_value(std::int64_t current) {
    if (current == std::numeric_limits<std::int64_t>::max()) {
        throw std::overflow_error("ID or sequence counter exhausted");
    }
    return current + 1;
}

void validate(const SubmitOrder& request) {
    if ((request.side != Side::Buy && request.side != Side::Sell)
        || request.quantity <= 0 || (request.id && *request.id <= 0)) {
        throw std::invalid_argument("invalid side, quantity, or order ID");
    }
    if (request.type == OrderType::Limit) {
        if (!request.price || *request.price <= 0) {
            throw std::invalid_argument("limit orders require a positive price");
        }
    } else if (request.type != OrderType::Market || request.price) {
        throw std::invalid_argument("invalid order type or price supplied for market order");
    }
}

} // namespace

void MatchingEngine::ensure_healthy() const {
    if (failed_) {
        throw std::runtime_error("engine unavailable after an internal commit failure");
    }
}

void MatchingEngine::reserve_trades(std::size_t additional) {
    if (additional > trades_.max_size() - trades_.size()) {
        throw std::length_error("trade history capacity exhausted");
    }
    const auto required = trades_.size() + additional;
    if (required > trades_.capacity()) {
        const auto doubled = trades_.capacity() > trades_.max_size() / 2
            ? trades_.max_size() : trades_.capacity() * 2;
        trades_.reserve(std::max(required, doubled));
    }
}

ExecutionResult MatchingEngine::submit(const SubmitOrder& request) {
    ensure_healthy();
    validate(request);
    const OrderId id = request.id ? *request.id : next_value(last_order_id_);
    if (book_.orders_.contains(id)) {
        throw DuplicateOrderId("duplicate order ID");
    }
    auto sequence = next_value(last_sequence_);
    auto trade_id = last_trade_id_;
    ExecutionResult result{Order{
        .id = id, .side = request.side, .type = request.type, .price = request.price,
        .original_quantity = request.quantity, .remaining_quantity = request.quantity,
        .executed_quantity = 0, .creation_sequence = sequence, .priority_sequence = sequence,
        .created_at = request.timestamp, .updated_at = request.timestamp,
        .status = OrderStatus::New
    }, {}};
    plan_matches(result, sequence, trade_id);
    reserve_trades(result.trades.size());
    const auto& incoming = result.order;
    if (incoming.type == OrderType::Limit && incoming.remaining_quantity > 0) {
        book_.add_resting_impl(incoming, false);
    } else {
        book_.orders_.emplace(id, incoming);
    }
    commit_trades(result, sequence, trade_id);
    return result;
}

void MatchingEngine::plan_matches(ExecutionResult& result, SequenceNumber& sequence,
                                  TradeId& trade_id) const {
    auto& incoming = result.order;

    // Planning stores IDs, never hash-table iterators that staging could invalidate.
    const auto plan = [&](const auto& levels) {
        for (const auto& [price, level] : levels) {
            if (incoming.remaining_quantity == 0) {
                break;
            }
            if (incoming.price && (incoming.side == Side::Buy ? price > *incoming.price
                                                             : price < *incoming.price)) {
                break;
            }
            for (const auto maker_id : level.orders) {
                if (incoming.remaining_quantity == 0) {
                    break;
                }
                const auto& maker = book_.orders_.at(maker_id);
                const Quantity quantity = std::min(incoming.remaining_quantity, maker.remaining_quantity);
                trade_id = next_value(trade_id);
                sequence = next_value(sequence);
                result.trades.push_back(Trade{
                    .id = trade_id, .maker_order_id = maker_id, .taker_order_id = incoming.id,
                    .price = price, .quantity = quantity,
                    .execution_sequence = sequence, .executed_at = incoming.updated_at
                });
                incoming.remaining_quantity -= quantity;
                incoming.executed_quantity += quantity;
            }
        }
    };
    if (incoming.side == Side::Buy) {
        plan(book_.asks_);
    } else {
        plan(book_.bids_);
    }

    if (incoming.remaining_quantity == 0) {
        incoming.status = OrderStatus::Filled;
    } else if (incoming.type == OrderType::Market) {
        incoming.status = OrderStatus::Cancelled;
    } else if (incoming.executed_quantity > 0) {
        incoming.status = OrderStatus::PartiallyFilled;
    } else {
        incoming.status = OrderStatus::New;
    }
}

void MatchingEngine::commit_trades(const ExecutionResult& result, SequenceNumber sequence,
                                   TradeId trade_id) {
    try {
        const auto apply = [&](auto& levels) {
            for (const auto& trade : result.trades) {
                const auto level_it = levels.begin();
                if (level_it == levels.end() || level_it->first != trade.price
                    || level_it->second.orders.empty()
                    || level_it->second.orders.front() != trade.maker_order_id) {
                    throw std::logic_error("fill plan no longer agrees with the book");
                }
                auto& level = level_it->second;
                auto& maker = book_.orders_.at(trade.maker_order_id);
                maker.remaining_quantity -= trade.quantity;
                maker.executed_quantity += trade.quantity;
                maker.updated_at = trade.executed_at;
                level.total_quantity -= trade.quantity;
                maker.status = maker.remaining_quantity == 0
                    ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
                if (maker.remaining_quantity == 0) {
                    book_.locations_.erase(maker.id);
                    level.orders.pop_front();
                    if (level.orders.empty()) {
                        levels.erase(level_it);
                    }
                }
            }
        };
        if (result.order.side == Side::Buy) {
            apply(book_.asks_);
        } else {
            apply(book_.bids_);
        }
        static_assert(std::is_nothrow_copy_constructible_v<Trade>);
        static_assert(std::is_nothrow_move_constructible_v<ExecutionResult>);
        for (const auto& trade : result.trades) {
            trades_.push_back(trade); // Capacity was reserved before staging.
        }
        last_order_id_ = std::max(last_order_id_, result.order.id);
        last_trade_id_ = trade_id;
        last_sequence_ = sequence;
    } catch (...) {
        failed_ = true;
        throw;
    }
}

Order& MatchingEngine::active_order(OrderId id) {
    ensure_healthy();
    if (id <= 0) {
        throw std::invalid_argument("order ID must be positive");
    }
    const auto it = book_.orders_.find(id);
    if (it == book_.orders_.end()) {
        throw UnknownOrderId("unknown order ID");
    }
    if (!book_.locations_.contains(id)) {
        throw OrderNotActive("order is already filled or cancelled");
    }
    return it->second;
}

Order MatchingEngine::cancel(OrderId id, Timestamp timestamp) {
    auto& order = active_order(id);
    const auto sequence = next_value(last_sequence_);
    try {
        book_.remove_active(id);
        order.status = OrderStatus::Cancelled;
        order.updated_at = timestamp;
        last_sequence_ = sequence;
    } catch (...) {
        failed_ = true;
        throw;
    }
    return order;
}

ExecutionResult MatchingEngine::modify(OrderId id, const ModifyOrder& request) {
    const auto& previous = active_order(id);
    if ((!request.price && !request.remaining_quantity)
        || (request.price && *request.price <= 0)
        || (request.remaining_quantity && *request.remaining_quantity <= 0)) {
        throw std::invalid_argument("amendment requires a positive price or remaining quantity");
    }
    const auto price = request.price.value_or(*previous.price);
    const auto remaining = request.remaining_quantity.value_or(previous.remaining_quantity);
    if (remaining > std::numeric_limits<Quantity>::max() - previous.executed_quantity) {
        throw std::overflow_error("amended quantity exceeds int64 range");
    }
    if (price == previous.price && remaining == previous.remaining_quantity) {
        return {previous, {}};
    }

    auto sequence = next_value(last_sequence_);
    auto trade_id = last_trade_id_;
    const bool retain_priority = price == previous.price && remaining < previous.remaining_quantity;
    ExecutionResult result{previous, {}};
    auto& amended = result.order;
    amended.price = price;
    amended.remaining_quantity = remaining;
    amended.quantity_adjustment = amended.executed_quantity + remaining - amended.original_quantity;
    amended.updated_at = request.timestamp;
    if (!retain_priority) {
        amended.priority_sequence = sequence;
        plan_matches(result, sequence, trade_id);
    }
    reserve_trades(result.trades.size());
    book_.replace_active(amended, retain_priority);
    commit_trades(result, sequence, trade_id);
    return result;
}

std::optional<Order> MatchingEngine::find_order(OrderId id) const {
    ensure_healthy();
    return book_.find_order(id);
}

std::vector<PriceLevelSnapshot> MatchingEngine::bids() const {
    ensure_healthy();
    return book_.bids();
}

std::vector<PriceLevelSnapshot> MatchingEngine::asks() const {
    ensure_healthy();
    return book_.asks();
}

std::size_t MatchingEngine::active_order_count() const {
    ensure_healthy();
    return book_.active_order_count();
}

std::vector<Trade> MatchingEngine::trades(TradeId after_id, std::size_t limit) const {
    ensure_healthy();
    if (after_id < 0) {
        throw std::invalid_argument("trade cursor must be nonnegative");
    }
    const auto begin = std::upper_bound(trades_.begin(), trades_.end(), after_id,
        [](TradeId cursor, const Trade& trade) { return cursor < trade.id; });
    const auto count = std::min(limit, static_cast<std::size_t>(trades_.end() - begin));
    return {begin, begin + static_cast<std::ptrdiff_t>(count)};
}

void MatchingEngine::check_invariants() const {
    ensure_healthy();
    book_.check_invariants();
    std::unordered_map<OrderId, Quantity> executed;
    TradeId previous_id = 0;
    SequenceNumber previous_sequence = 0;
    for (const auto& trade : trades_) {
        const auto& maker = book_.orders_.at(trade.maker_order_id);
        const auto& taker = book_.orders_.at(trade.taker_order_id);
        if (trade.id <= previous_id || trade.execution_sequence <= previous_sequence
            || trade.execution_sequence > last_sequence_ || trade.quantity <= 0
            || trade.price <= 0 || maker.side == taker.side || maker.type != OrderType::Limit
            || trade.execution_sequence <= maker.creation_sequence
            || trade.execution_sequence <= taker.creation_sequence) {
            throw std::logic_error("invalid trade history");
        }
        for (const auto order_id : {trade.maker_order_id, trade.taker_order_id}) {
            auto& quantity = executed[order_id];
            if (trade.quantity > std::numeric_limits<Quantity>::max() - quantity) {
                throw std::logic_error("executed quantity overflow");
            }
            quantity += trade.quantity;
        }
        previous_id = trade.id;
        previous_sequence = trade.execution_sequence;
    }
    if (previous_id != last_trade_id_) {
        throw std::logic_error("trade ID counter mismatch");
    }
    for (const auto& [id, order] : book_.orders_) {
        if (id > last_order_id_ || order.priority_sequence > last_sequence_
            || executed[id] != order.executed_quantity
            || order.executed_quantity + order.remaining_quantity - order.original_quantity
                != order.quantity_adjustment) {
            throw std::logic_error("order counters or execution accounting mismatch");
        }
    }
}

} // namespace orderbook
