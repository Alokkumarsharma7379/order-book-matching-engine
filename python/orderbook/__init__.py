"""Python interface to the compiled matching engine."""
import os
from pathlib import Path

if os.name == "nt":
    _runtime_directory = os.add_dll_directory(str(Path(__file__).resolve().parent))

from ._core import (  # noqa: E402,F401
    DuplicateOrderId, ExecutionResult, MatchingEngine, Order, OrderNotActive,
    OrderStatus, OrderType, PriceLevel, Side, Trade, UnknownOrderId,
)
