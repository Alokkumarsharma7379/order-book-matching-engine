from datetime import datetime, timezone, timedelta
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator

MAX_INT = (1 << 63) - 1
PositiveInt64 = Annotated[int, Field(strict=True, gt=0, le=MAX_INT)]
EPOCH = datetime(1970, 1, 1, tzinfo=timezone.utc)


class SubmitOrder(BaseModel):
    model_config = ConfigDict(extra="forbid")
    side: Literal["BUY", "SELL"]
    type: Literal["LIMIT", "MARKET"]
    price: PositiveInt64 | None = None
    quantity: PositiveInt64
    id: PositiveInt64 | None = None

    @model_validator(mode="after")
    def valid_price(self):
        if self.type == "LIMIT" and self.price is None:
            raise ValueError("LIMIT orders require a price")
        if self.type == "MARKET" and "price" in self.model_fields_set:
            raise ValueError("MARKET orders must omit price")
        return self


class ModifyOrder(BaseModel):
    model_config = ConfigDict(extra="forbid")
    price: PositiveInt64 | None = None
    remaining_quantity: PositiveInt64 | None = None

    @model_validator(mode="after")
    def valid_amendment(self):
        if not self.model_fields_set or any(getattr(self, key) is None for key in self.model_fields_set):
            raise ValueError("supply a positive price or remaining_quantity; null is not an amendment")
        return self


class OrderView(BaseModel):
    id: int
    side: Literal["BUY", "SELL"]
    type: Literal["LIMIT", "MARKET"]
    price: int | None
    original_quantity: int
    remaining_quantity: int
    executed_quantity: int
    quantity_adjustment: int
    creation_sequence: int
    priority_sequence: int
    created_at: datetime
    updated_at: datetime
    status: Literal["NEW", "PARTIALLY_FILLED", "FILLED", "CANCELLED"]


class TradeView(BaseModel):
    id: int
    maker_order_id: int
    taker_order_id: int
    price: int
    quantity: int
    execution_sequence: int
    executed_at: datetime


class ExecutionView(BaseModel):
    order: OrderView
    trades: list[TradeView]


class LevelView(BaseModel):
    price: int
    total_quantity: int
    order_count: int


class BookView(BaseModel):
    symbol: Literal["DEMO"] = "DEMO"
    bids: list[LevelView]
    asks: list[LevelView]


def iso(microseconds: int) -> str:
    return (EPOCH + timedelta(microseconds=microseconds)).isoformat()


def order_dict(order) -> dict:
    return {
        "id": order.id, "side": order.side.name, "type": order.type.name, "price": order.price,
        **{key: getattr(order, key) for key in (
            "original_quantity", "remaining_quantity", "executed_quantity", "quantity_adjustment",
            "creation_sequence", "priority_sequence",
        )},
        "created_at": iso(order.created_at_us), "updated_at": iso(order.updated_at_us),
        "status": order.status.name,
    }


def trade_dict(trade) -> dict:
    return {**{key: getattr(trade, key) for key in (
        "id", "maker_order_id", "taker_order_id", "price", "quantity", "execution_sequence",
    )}, "executed_at": iso(trade.executed_at_us)}


def level_dict(level) -> dict:
    return {"price": level.price, "total_quantity": level.total_quantity, "order_count": level.order_count}
