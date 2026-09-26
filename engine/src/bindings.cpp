#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "orderbook/matching_engine.hpp"

namespace py = pybind11;
using namespace orderbook;

namespace {
Timestamp timestamp(std::int64_t microseconds) {
    return Timestamp{std::chrono::microseconds{microseconds}};
}
std::int64_t micros(Timestamp value) { return value.time_since_epoch().count(); }
} // namespace

PYBIND11_MODULE(_core, module) {
    module.doc() = "Single-instrument C++20 matching engine; timestamps are Unix microseconds.";
    py::enum_<Side>(module, "Side").value("BUY", Side::Buy).value("SELL", Side::Sell);
    py::enum_<OrderType>(module, "OrderType")
        .value("LIMIT", OrderType::Limit).value("MARKET", OrderType::Market);
    py::enum_<OrderStatus>(module, "OrderStatus")
        .value("NEW", OrderStatus::New).value("PARTIALLY_FILLED", OrderStatus::PartiallyFilled)
        .value("FILLED", OrderStatus::Filled).value("CANCELLED", OrderStatus::Cancelled);
    py::register_exception<DuplicateOrderId>(module, "DuplicateOrderId", PyExc_ValueError);
    py::register_exception<UnknownOrderId>(module, "UnknownOrderId", PyExc_KeyError);
    py::register_exception<OrderNotActive>(module, "OrderNotActive", PyExc_ValueError);

    py::class_<Order>(module, "Order")
        .def_readonly("id", &Order::id).def_readonly("side", &Order::side)
        .def_readonly("type", &Order::type).def_readonly("price", &Order::price)
        .def_readonly("original_quantity", &Order::original_quantity)
        .def_readonly("remaining_quantity", &Order::remaining_quantity)
        .def_readonly("executed_quantity", &Order::executed_quantity)
        .def_readonly("quantity_adjustment", &Order::quantity_adjustment)
        .def_readonly("creation_sequence", &Order::creation_sequence)
        .def_readonly("priority_sequence", &Order::priority_sequence)
        .def_property_readonly("created_at_us", [](const Order& o) { return micros(o.created_at); })
        .def_property_readonly("updated_at_us", [](const Order& o) { return micros(o.updated_at); })
        .def_readonly("status", &Order::status);
    py::class_<Trade>(module, "Trade")
        .def_readonly("id", &Trade::id).def_readonly("maker_order_id", &Trade::maker_order_id)
        .def_readonly("taker_order_id", &Trade::taker_order_id).def_readonly("price", &Trade::price)
        .def_readonly("quantity", &Trade::quantity)
        .def_readonly("execution_sequence", &Trade::execution_sequence)
        .def_property_readonly("executed_at_us", [](const Trade& t) { return micros(t.executed_at); });
    py::class_<PriceLevelSnapshot>(module, "PriceLevel")
        .def_readonly("price", &PriceLevelSnapshot::price)
        .def_readonly("total_quantity", &PriceLevelSnapshot::total_quantity)
        .def_readonly("order_count", &PriceLevelSnapshot::order_count);
    py::class_<ExecutionResult>(module, "ExecutionResult")
        .def_readonly("order", &ExecutionResult::order).def_readonly("trades", &ExecutionResult::trades);

    // Keep the GIL. The API additionally locks the whole command + persistence boundary.
    py::class_<MatchingEngine>(module, "MatchingEngine")
        .def(py::init<>())
        .def("submit", [](MatchingEngine& engine, Side side, OrderType type,
                           std::optional<Price> price, Quantity quantity, std::int64_t timestamp_us,
                           std::optional<OrderId> id) {
            return engine.submit({side, type, price, quantity, timestamp(timestamp_us), id});
        }, py::arg("side"), py::arg("type"), py::arg("price"), py::arg("quantity"),
           py::arg("timestamp_us"), py::arg("id") = py::none())
        .def("cancel", [](MatchingEngine& engine, OrderId id, std::int64_t timestamp_us) {
            return engine.cancel(id, timestamp(timestamp_us));
        }, py::arg("id"), py::arg("timestamp_us"))
        .def("modify", [](MatchingEngine& engine, OrderId id, std::optional<Price> price,
                           std::optional<Quantity> remaining_quantity, std::int64_t timestamp_us) {
            return engine.modify(id, {price, remaining_quantity, timestamp(timestamp_us)});
        }, py::arg("id"), py::arg("price"), py::arg("remaining_quantity"), py::arg("timestamp_us"))
        .def("find_order", &MatchingEngine::find_order)
        .def("bids", &MatchingEngine::bids).def("asks", &MatchingEngine::asks)
        .def("trades", &MatchingEngine::trades, py::arg("after_id") = 0, py::arg("limit") = 100)
        .def_property_readonly("active_order_count", &MatchingEngine::active_order_count)
        .def_property_readonly("healthy", &MatchingEngine::healthy)
        .def("check_invariants", &MatchingEngine::check_invariants);
}
