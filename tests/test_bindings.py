import gc

import pytest
import orderbook as ob


def test_compiled_engine_lifetime_and_snapshot():
    engine = ob.MatchingEngine()
    result = engine.submit(ob.Side.SELL, ob.OrderType.LIMIT, 100, 5, -1)
    engine.submit(ob.Side.BUY, ob.OrderType.MARKET, None, 2, 8)
    assert result.order.remaining_quantity == 5
    assert engine.find_order(1).remaining_quantity == 3
    del engine
    gc.collect()
    assert result.order.created_at_us == -1
    with pytest.raises(AttributeError):
        result.order.price = 5


def test_typed_errors_and_int64_bounds():
    engine = ob.MatchingEngine()
    maximum = 2**63 - 1
    assert engine.submit(ob.Side.BUY, ob.OrderType.LIMIT, maximum, maximum, 1, 7).order.id == 7
    with pytest.raises(ob.DuplicateOrderId):
        engine.submit(ob.Side.BUY, ob.OrderType.LIMIT, 1, 1, 1, 7)
    with pytest.raises(ob.UnknownOrderId):
        engine.cancel(9, 2)
    for quantity in (maximum + 1, 1.5):
        with pytest.raises((TypeError, OverflowError)):
            engine.submit(ob.Side.BUY, ob.OrderType.LIMIT, 1, quantity, 2)
    engine.cancel(7, 3)
    with pytest.raises(ob.OrderNotActive):
        engine.modify(7, 1, 1, 4)
    engine.check_invariants()


def test_binding_amendment_and_trade_fields():
    engine = ob.MatchingEngine()
    engine.submit(ob.Side.BUY, ob.OrderType.LIMIT, 99, 4, 1)
    engine.submit(ob.Side.SELL, ob.OrderType.LIMIT, 100, 5, 2)
    result = engine.modify(1, 100, 6, 3)
    assert result.order.remaining_quantity == 1
    assert result.order.quantity_adjustment == 2
    assert result.trades[0].maker_order_id == 2
    assert result.trades[0].executed_at_us == 3
    assert engine.trades(1, 10) == []
    assert engine.bids()[0].total_quantity == 1
    assert engine.active_order_count == 1
    engine.check_invariants()
