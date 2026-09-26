#include "orderbook/matching_engine.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace orderbook;
using Clock = std::chrono::steady_clock;

namespace {
struct Command { bool cancel; Side side; Price price; Quantity quantity; OrderId id; };

std::vector<Command> workload(std::string_view name, std::size_t count) {
    std::vector<Command> commands;
    commands.reserve(count);
    OrderId cancelled = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (name == "mostly_noncrossing") {
            const bool buy = i % 2 == 0;
            // Every 20th command crosses; others build a two-sided book.
            commands.push_back({false, buy ? Side::Buy : Side::Sell,
                i % 20 == 19 ? 90 : buy ? 90 + static_cast<Price>(i % 10)
                                            : 101 + static_cast<Price>(i % 10), 10, 0});
        } else if (name == "frequent_crossing") {
            commands.push_back({false, i % 2 == 0 ? Side::Sell : Side::Buy, 100, 10, 0});
        } else if (name == "same_price") {
            commands.push_back({false, Side::Buy, 100, 1, 0});
        } else if (name == "many_levels") {
            commands.push_back({false, Side::Buy, static_cast<Price>(i + 1), 1, 0});
        } else {
            const bool cancel = i % 4 != 0;
            commands.push_back({cancel, Side::Buy, 100, 1, cancel ? ++cancelled : 0});
        }
    }
    return commands;
}

void prepare(MatchingEngine& engine, std::string_view scenario, std::size_t count) {
    if (scenario == "heavy_cancellation") {
        for (std::size_t i = 0; i < count; ++i) {
            (void)engine.submit({Side::Buy, OrderType::Limit, 100, 1, Timestamp{}, std::nullopt});
        }
    }
}

struct Observation { std::size_t trades{}; std::uint64_t checksum{}; };

void apply(MatchingEngine& engine, const Command& command, std::size_t i, Observation& observation) {
    const auto timestamp = Timestamp{std::chrono::microseconds{static_cast<std::int64_t>(i)}};
    if (command.cancel) {
        const auto order = engine.cancel(command.id, timestamp);
        observation.checksum += static_cast<std::uint64_t>(order.id);
    } else {
        const auto result = engine.submit({command.side, OrderType::Limit, command.price,
                                          command.quantity, timestamp, std::nullopt});
        observation.trades += result.trades.size();
        observation.checksum += static_cast<std::uint64_t>(result.order.id + result.order.remaining_quantity);
    }
}

void warmup(std::string_view scenario, std::size_t count) {
    MatchingEngine engine;
    prepare(engine, scenario, count);
    const auto commands = workload(scenario, count);
    Observation observation;
    for (std::size_t i = 0; i < commands.size(); ++i) { apply(engine, commands[i], i, observation); }
    engine.check_invariants();
}

std::int64_t percentile(const std::vector<std::int64_t>& sorted, double fraction) {
    const auto rank = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(sorted.size())));
    return sorted[std::max(std::size_t{1}, rank) - 1];
}

std::size_t number(std::string_view text) {
    std::size_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 || value > 10000000) {
        throw std::invalid_argument("arguments must be positive integers no larger than 10000000");
    }
    return value;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 4) { throw std::invalid_argument("usage: orderbook_benchmark [commands=50000] [trials=5] [warmup=5000]"); }
        const auto count = argc > 1 ? number(argv[1]) : 50000;
        const auto trials = argc > 2 ? number(argv[2]) : 5;
        const auto warmup_count = argc > 3 ? number(argv[3]) : 5000;
        if (trials > 100) { throw std::invalid_argument("trial count must be at most 100"); }
#ifdef NDEBUG
        std::cerr << "Build: Release (NDEBUG); ";
#else
        std::cerr << "Build: Debug; do not use for performance claims; ";
#endif
#ifdef __clang__
        std::cerr << "compiler: Clang " << __clang_version__ << '\n';
#elif defined(__GNUC__)
        std::cerr << "compiler: GCC " << __VERSION__ << '\n';
#elif defined(_MSC_VER)
        std::cerr << "compiler: MSVC " << _MSC_VER << '\n';
#endif
        std::cout << "scenario,trial,commands,submissions,cancellations,trades,seconds,commands_per_second,"
                     "orders_per_second,mean_ns,median_ns,p95_ns,p99_ns,checksum\n" << std::fixed << std::setprecision(9);
        for (const auto scenario : {"mostly_noncrossing", "frequent_crossing", "same_price", "many_levels", "heavy_cancellation"}) {
            warmup(scenario, warmup_count);
            const auto commands = workload(scenario, count);
            const auto cancellations = static_cast<std::size_t>(std::count_if(commands.begin(), commands.end(),
                [](const auto& command) { return command.cancel; }));
            for (std::size_t trial = 1; trial <= trials; ++trial) {
                MatchingEngine engine;
                prepare(engine, scenario, count);
                std::vector<std::int64_t> latency;
                latency.reserve(count);
                Observation observation;
                const auto start = Clock::now();
                for (std::size_t i = 0; i < commands.size(); ++i) {
                    const auto begin = Clock::now();
                    apply(engine, commands[i], i, observation);
                    const auto end = Clock::now();
                    latency.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
                }
                const auto stop = Clock::now();
                engine.check_invariants(); // Excluded from timing, never disabled by NDEBUG.
                const double seconds = std::chrono::duration<double>(stop - start).count();
                const auto mean = std::accumulate(latency.begin(), latency.end(), 0.0) / static_cast<double>(count);
                std::sort(latency.begin(), latency.end());
                std::cout << scenario << ',' << trial << ',' << count << ',' << count - cancellations << ','
                    << cancellations << ',' << observation.trades << ',' << seconds << ','
                    << static_cast<double>(count) / seconds << ','
                    << static_cast<double>(count - cancellations) / seconds << ',' << mean << ','
                    << percentile(latency, 0.5) << ',' << percentile(latency, 0.95) << ','
                    << percentile(latency, 0.99) << ',' << observation.checksum << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
