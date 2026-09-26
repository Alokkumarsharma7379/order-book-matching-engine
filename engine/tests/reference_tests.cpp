#include "test_support.hpp"
#include "reference_model.hpp"

#include <random>

using namespace test_support;

TEST_CASE("Mixed commands agree with an independent linear reference model", "[reference][invariant]") {
    const auto seed = GENERATE(7U, 42U, 2026U, 0xC0FFEEU);
    std::mt19937 random(seed);
    MatchingEngine engine;
    ReferenceModel model;
    std::size_t submissions = 0;
    std::size_t cancellations = 0;
    std::size_t amendments = 0;
    for (int step = 0; step < 600; ++step) {
        const auto active = model.active_ids();
        const auto operation = random() % 10;
        const auto timestamp = stamp(static_cast<std::int64_t>(random() % 21) - 10);
        INFO("seed=" << seed << " step=" << step << " operation=" << operation);
        if (active.empty() || operation < 6) {
            const auto side = random() % 2 == 0 ? Side::Buy : Side::Sell;
            const auto type = random() % 5 == 0 ? OrderType::Market : OrderType::Limit;
            const auto price = type == OrderType::Limit
                ? std::optional<Price>{95 + static_cast<Price>(random() % 11)} : std::nullopt;
            const Quantity quantity = 1 + static_cast<Quantity>(random() % 40);
            const SubmitOrder request{side, type, price, quantity, timestamp, std::nullopt};
            const auto expected = model.submit(request);
            same_result(engine.submit(request), expected);
            ++submissions;
        } else {
            const auto id = active[random() % active.size()];
            if (operation < 8) {
                const auto expected = model.cancel(id, timestamp);
                REQUIRE(order_fields(engine.cancel(id, timestamp)) == order_fields(expected));
                ++cancellations;
            } else {
                const auto mode = random() % 4;
                const auto previous = *std::find_if(model.records().begin(), model.records().end(),
                    [&](const auto& order) { return order.id == id; });
                ModifyOrder request;
                request.timestamp = timestamp;
                if (mode == 3) {
                    request.price = previous.price;
                    request.remaining_quantity = previous.remaining_quantity;
                } else {
                    if (mode != 1) { request.price = 95 + static_cast<Price>(random() % 11); }
                    if (mode != 0) { request.remaining_quantity = 1 + static_cast<Quantity>(random() % 40); }
                }
                const auto expected = model.modify(id, request);
                same_result(engine.modify(id, request), expected);
                ++amendments;
            }
        }

        REQUIRE(engine.healthy());
        REQUIRE_NOTHROW(engine.check_invariants());
        REQUIRE(engine.active_order_count() == model.active_ids().size());
        REQUIRE(level_fields(engine.bids()) == level_fields(model.levels(Side::Buy)));
        REQUIRE(level_fields(engine.asks()) == level_fields(model.levels(Side::Sell)));
        for (const auto& expected : model.records()) {
            const auto actual = engine.find_order(expected.id);
            REQUIRE(actual.has_value());
            REQUIRE(order_fields(*actual) == order_fields(expected));
        }
        const auto actual_trades = engine.trades(0, std::numeric_limits<std::size_t>::max());
        REQUIRE(actual_trades.size() == model.trades().size());
        for (std::size_t i = 0; i < actual_trades.size(); ++i) {
            REQUIRE(trade_fields(actual_trades[i]) == trade_fields(model.trades()[i]));
        }
    }
    CHECK(submissions > 0);
    CHECK(cancellations > 0);
    CHECK(amendments > 0);
}
