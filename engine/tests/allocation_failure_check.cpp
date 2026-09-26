#include "orderbook/matching_engine.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>

// Isolate the replacement allocator from Catch2 and its own allocations.
// Only ordinary new/new[] calls in the armed command are intercepted.
namespace allocation_fault {
thread_local std::ptrdiff_t countdown = -1;

void* allocate(std::size_t size) {
    if (countdown == 0) {
        countdown = -1;
        throw std::bad_alloc{};
    }
    if (countdown > 0) { --countdown; }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) { return memory; }
    throw std::bad_alloc{};
}

struct Scope {
    explicit Scope(std::ptrdiff_t index) { countdown = index; }
    ~Scope() { countdown = -1; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
} // namespace allocation_fault

void* operator new(std::size_t size) { return allocation_fault::allocate(size); }
void* operator new[](std::size_t size) { return allocation_fault::allocate(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace orderbook;

Timestamp stamp(std::int64_t value) { return Timestamp{std::chrono::microseconds{value}}; }
Side opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }
Price own_price(Side side) { return side == Side::Buy ? 90 : 110; }
Price crossing_price(Side side) { return side == Side::Buy ? 103 : 97; }

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

auto order_fields(const Order& order) {
    return std::tuple{order.id, order.side, order.type, order.price, order.original_quantity,
        order.remaining_quantity, order.executed_quantity, order.quantity_adjustment,
        order.creation_sequence, order.priority_sequence, order.created_at,
        order.updated_at, order.status};
}

auto snapshot(const MatchingEngine& engine) {
    using Fields = decltype(order_fields(Order{}));
    std::vector<std::optional<Fields>> orders;
    // All fixtures and their replay probes use at most seven generated IDs.
    for (OrderId id = 1; id <= 16; ++id) {
        const auto order = engine.find_order(id);
        orders.push_back(order ? std::optional<Fields>{order_fields(*order)} : std::nullopt);
    }
    std::vector<std::tuple<TradeId, OrderId, OrderId, Price, Quantity, SequenceNumber, Timestamp>> trades;
    for (const auto& trade : engine.trades(0, std::numeric_limits<std::size_t>::max())) {
        trades.emplace_back(trade.id, trade.maker_order_id, trade.taker_order_id, trade.price,
                            trade.quantity, trade.execution_sequence, trade.executed_at);
    }
    std::vector<std::tuple<Side, Price, Quantity, std::size_t>> levels;
    for (const auto side : {Side::Buy, Side::Sell}) {
        for (const auto& level : side == Side::Buy ? engine.bids() : engine.asks()) {
            levels.emplace_back(side, level.price, level.total_quantity, level.order_count);
        }
    }
    return std::tuple{orders, trades, levels, engine.active_order_count(), engine.healthy()};
}

enum class Scenario {
    NewLevel, ExistingLevel, MarketSweep, LimitRemainder, EmptyMarket,
    Reprice, AmendSweep, AmendRemainder, Cancel, Reduce, Increase, NoOp
};

void submit(MatchingEngine& engine, Side side, Price price, Quantity quantity) {
    (void)engine.submit({side, OrderType::Limit, price, quantity, stamp(10), std::nullopt});
}

void setup(MatchingEngine& engine, Side side, Scenario scenario) {
    if (scenario == Scenario::EmptyMarket) { return; }
    submit(engine, side, own_price(side), 4);
    submit(engine, side, own_price(side), 5);
    submit(engine, opposite(side), 100, 3);
    submit(engine, opposite(side), side == Side::Buy ? 101 : 99, 4);
}

void command(MatchingEngine& engine, Side side, Scenario scenario) {
    const Price nearby = side == Side::Buy ? 91 : 109;
    switch (scenario) {
    case Scenario::NewLevel: submit(engine, side, nearby, 2); break;
    case Scenario::ExistingLevel: submit(engine, side, own_price(side), 2); break;
    case Scenario::MarketSweep:
    case Scenario::EmptyMarket:
        (void)engine.submit({side, OrderType::Market, std::nullopt, 7, stamp(20), std::nullopt});
        break;
    case Scenario::LimitRemainder: submit(engine, side, crossing_price(side), 10); break;
    case Scenario::Reprice: (void)engine.modify(1, {nearby, std::nullopt, stamp(30)}); break;
    case Scenario::AmendSweep: (void)engine.modify(1, {crossing_price(side), 7, stamp(30)}); break;
    case Scenario::AmendRemainder: (void)engine.modify(1, {crossing_price(side), 10, stamp(30)}); break;
    case Scenario::Cancel: (void)engine.cancel(1, stamp(30)); break;
    case Scenario::Reduce: (void)engine.modify(1, {std::nullopt, 2, stamp(30)}); break;
    case Scenario::Increase: (void)engine.modify(1, {std::nullopt, 6, stamp(30)}); break;
    case Scenario::NoOp: (void)engine.modify(1, {own_price(side), 4, stamp(30)}); break;
    }
}

void probe_counters(MatchingEngine& engine, Side side) {
    submit(engine, side, own_price(side), 2);
    (void)engine.submit({opposite(side), OrderType::Market, std::nullopt, 1, stamp(40), std::nullopt});
}

std::size_t check(Scenario scenario, Side side, bool expect_allocations) {
    std::size_t failures = 0;
    for (std::ptrdiff_t index = 0; index < 128; ++index) {
        MatchingEngine engine;
        MatchingEngine control;
        setup(engine, side, scenario);
        setup(control, side, scenario);
        const auto before = snapshot(engine);
        bool failed = false;
        try {
            const allocation_fault::Scope fault(index);
            command(engine, side, scenario);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        require(engine.healthy(), "An allocation failure poisoned the engine");
        engine.check_invariants();
        if (failed) {
            ++failures;
            require(snapshot(engine) == before, "An allocation failure changed logical state");
            command(engine, side, scenario);
        }
        command(control, side, scenario);
        probe_counters(engine, side);
        probe_counters(control, side);
        engine.check_invariants();
        require(snapshot(engine) == snapshot(control), "Replay changed state or consumed counters");
        if (!failed) {
            require(expect_allocations == (failures > 0), "Unexpected command allocation behavior");
            return failures;
        }
    }
    throw std::runtime_error("Allocation failure sweep did not reach a successful command");
}
} // namespace

int main() {
    try {
        std::size_t failure_points = 0;
        for (const auto side : {Side::Buy, Side::Sell}) {
            for (const auto scenario : {Scenario::NewLevel, Scenario::ExistingLevel,
                    Scenario::MarketSweep, Scenario::LimitRemainder, Scenario::EmptyMarket,
                    Scenario::Reprice, Scenario::AmendSweep, Scenario::AmendRemainder}) {
                failure_points += check(scenario, side, true);
            }
            for (const auto scenario : {Scenario::Cancel, Scenario::Reduce,
                                       Scenario::Increase, Scenario::NoOp}) {
                failure_points += check(scenario, side, false);
            }
        }
        std::cout << "Allocation failure checks passed (" << failure_points
                  << " injected failure points, 24 scenario/side combinations)\n";
    } catch (const std::exception& error) {
        std::cerr << "Allocation failure check failed: " << error.what() << '\n';
        return 1;
    }
}
