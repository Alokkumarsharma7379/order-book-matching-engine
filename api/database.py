"""A command journal and historical projections; no SQL in the C++ hot path."""
from datetime import datetime, timezone

import sqlalchemy as sa
from sqlalchemy.pool import NullPool

metadata = sa.MetaData()
orders = sa.Table(
    "orders", metadata,
    sa.Column("id", sa.BigInteger, primary_key=True, autoincrement=False),
    sa.Column("side", sa.String(4), nullable=False),
    sa.Column("order_type", sa.String(6), nullable=False),
    sa.Column("price", sa.BigInteger),
    *[sa.Column(name, sa.BigInteger, nullable=False) for name in (
        "original_quantity", "remaining_quantity", "executed_quantity", "quantity_adjustment",
        "creation_sequence", "priority_sequence",
    )],
    sa.Column("created_at", sa.DateTime(timezone=True), nullable=False),
    sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False),
    sa.Column("status", sa.String(16), nullable=False),
    sa.CheckConstraint("side IN ('BUY', 'SELL')"),
    sa.CheckConstraint("order_type IN ('LIMIT', 'MARKET')"),
    sa.CheckConstraint("(order_type = 'LIMIT' AND price IS NOT NULL AND price > 0) OR (order_type = 'MARKET' AND price IS NULL)"),
    sa.CheckConstraint("original_quantity > 0 AND remaining_quantity >= 0 AND executed_quantity >= 0"),
    sa.CheckConstraint("status IN ('NEW', 'PARTIALLY_FILLED', 'FILLED', 'CANCELLED')"),
)
trades = sa.Table(
    "trades", metadata,
    sa.Column("id", sa.BigInteger, primary_key=True, autoincrement=False),
    sa.Column("maker_order_id", sa.BigInteger, sa.ForeignKey("orders.id"), nullable=False),
    sa.Column("taker_order_id", sa.BigInteger, sa.ForeignKey("orders.id"), nullable=False),
    sa.Column("price", sa.BigInteger, nullable=False),
    sa.Column("quantity", sa.BigInteger, nullable=False),
    sa.Column("execution_sequence", sa.BigInteger, nullable=False, unique=True),
    sa.Column("executed_at", sa.DateTime(timezone=True), nullable=False),
    sa.CheckConstraint("price > 0 AND quantity > 0 AND maker_order_id <> taker_order_id"),
)
commands = sa.Table(
    "commands", metadata,
    sa.Column("id", sa.BigInteger, primary_key=True, autoincrement=False),
    sa.Column("version", sa.Integer, nullable=False),
    sa.Column("kind", sa.String(8), nullable=False),
    sa.Column("payload", sa.JSON, nullable=False),
    sa.Column("timestamp_us", sa.BigInteger, nullable=False),
    sa.Column("idempotency_key", sa.String(128), unique=True),
    sa.Column("fingerprint", sa.String(64), nullable=False),
    sa.Column("response", sa.JSON, nullable=False),
)


class Store:
    """One dedicated connection, owned by the service's serialization lock.

    A PostgreSQL session advisory lock prevents independent engine writers. Never
    silently replace a broken connection: that would lose ownership of the book.
    SQLite is accepted only when explicitly enabled by a test.
    """

    def __init__(self, url: str, *, allow_sqlite: bool = False):
        self.engine = sa.create_engine(url, poolclass=NullPool)
        if self.engine.dialect.name != "postgresql" and not allow_sqlite:
            self.engine.dispose()
            raise ValueError("persistent operation requires PostgreSQL")
        self.connection = self.engine.connect()
        try:
            if self.engine.dialect.name == "postgresql":
                with self.connection.begin():
                    owned = self.connection.scalar(sa.text("SELECT pg_try_advisory_lock(731904812)"))
                    if not owned:
                        raise RuntimeError("another process owns the DEMO engine; use one API worker")
            else:
                with self.connection.begin():
                    self.connection.exec_driver_sql("PRAGMA foreign_keys=ON")
        except BaseException:
            self.close()
            raise

    def close(self):
        self.connection.close()
        self.engine.dispose()

    def begin(self):
        if self.connection.invalidated or self.connection.closed:
            raise RuntimeError("database ownership connection was lost; restart required")
        return self.connection.begin()

    def ping(self):
        self.connection.execute(sa.text("SELECT 1"))

    def journal(self):
        with self.begin():
            return list(self.connection.execute(sa.select(commands).order_by(commands.c.id)).mappings())

    def verify(self, expected_orders: list[dict], expected_trades: list[dict]):
        def normalize(row):
            result = dict(row)
            if "order_type" in result:
                result["type"] = result.pop("order_type")
            for key, value in result.items():
                if isinstance(value, datetime):
                    if value.tzinfo is None:  # SQLite is only used in isolated tests.
                        value = value.replace(tzinfo=timezone.utc)
                    result[key] = value.astimezone(timezone.utc).isoformat()
            return result
        with self.begin():
            actual_orders = [normalize(row) for row in self.connection.execute(sa.select(orders).order_by(orders.c.id)).mappings()]
            actual_trades = [normalize(row) for row in self.connection.execute(sa.select(trades).order_by(trades.c.id)).mappings()]
        if actual_orders != expected_orders or actual_trades != expected_trades:
            raise RuntimeError("database projections disagree with the replayed journal")

    def save(self, command: dict, changed_orders: list[dict], new_trades: list[dict]):
        for order in changed_orders:
            values = {**order, "order_type": order["type"]}
            del values["type"]
            for key in ("created_at", "updated_at"):
                values[key] = datetime.fromisoformat(values[key])
            updated = self.connection.execute(orders.update().where(orders.c.id == order["id"]).values(**values))
            if updated.rowcount == 0:
                self.connection.execute(orders.insert().values(**values))
        if new_trades:
            values = [{**trade, "executed_at": datetime.fromisoformat(trade["executed_at"])} for trade in new_trades]
            self.connection.execute(trades.insert(), values)
        self.connection.execute(commands.insert().values(**command))
