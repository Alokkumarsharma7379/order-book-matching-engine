from contextlib import nullcontext
import hashlib
import json
import logging
import threading
import time

import orderbook as core

from .events import TradePublisher
from .schemas import level_dict, order_dict, trade_dict

log = logging.getLogger(__name__)


class Unavailable(RuntimeError):
    pass


class IdempotencyConflict(ValueError):
    pass


class EngineService:
    def __init__(self, store=None, publisher=None):
        self.lock = threading.RLock()
        self.engine = core.MatchingEngine()
        self.store = store
        self.publisher = publisher if publisher is not None else TradePublisher(None)
        self.failed = False
        self.last_command = 0
        self.idempotency = {}
        if store is not None:
            self._recover()

    def _apply(self, kind, payload, timestamp_us):
        if kind == "submit":
            result = self.engine.submit(
                core.Side.__members__[payload["side"]], core.OrderType.__members__[payload["type"]],
                payload.get("price"), payload["quantity"], timestamp_us, payload.get("id"),
            )
        elif kind == "modify":
            result = self.engine.modify(payload["id"], payload.get("price"),
                                        payload.get("remaining_quantity"), timestamp_us)
        elif kind == "cancel":
            return {"order": order_dict(self.engine.cancel(payload["id"], timestamp_us)), "trades": []}
        else:
            raise ValueError("unknown command kind")
        return {"order": order_dict(result.order), "trades": [trade_dict(trade) for trade in result.trades]}

    def _recover(self):
        order_ids = set()
        for command in self.store.journal():
            if command["version"] != 1 or command["id"] != self.last_command + 1:
                raise RuntimeError("unsupported or discontinuous command journal")
            actual = self._apply(command["kind"], command["payload"], command["timestamp_us"])
            if actual != command["response"]:
                raise RuntimeError("journal replay disagrees with recorded execution")
            order_ids.add(actual["order"]["id"])
            self.last_command = command["id"]
            if command["idempotency_key"]:
                self.idempotency[command["idempotency_key"]] = (command["fingerprint"], actual)
        self.engine.check_invariants()
        self.store.verify([order_dict(self.engine.find_order(id)) for id in sorted(order_ids)],
                          self.trades(limit=2**63 - 1))

    def _available(self):
        if self.failed or not self.engine.healthy:
            raise Unavailable("engine stopped after an uncertain failure; restart to replay committed commands")

    def execute(self, kind: str, payload: dict, key: str | None = None):
        fingerprint = hashlib.sha256(json.dumps([kind, payload], sort_keys=True).encode()).hexdigest()
        with self.lock:
            self._available()
            if key in self.idempotency:
                self._read_available()
                previous_fingerprint, response = self.idempotency[key]
                if fingerprint != previous_fingerprint:
                    raise IdempotencyConflict("Idempotency-Key was already used for a different command")
                return response
            timestamp_us = time.time_ns() // 1000
            applied = False
            try:
                with self.store.begin() if self.store is not None else nullcontext():
                    if self.store is not None:
                        self.store.ping()  # Verify ownership connection before changing memory.
                    response = self._apply(kind, payload, timestamp_us)
                    applied = True
                    if self.store is not None:
                        ids = {response["order"]["id"], *(t["maker_order_id"] for t in response["trades"])}
                        changed = [order_dict(self.engine.find_order(id)) for id in sorted(ids)]
                        self.store.save({
                            "id": self.last_command + 1, "version": 1, "kind": kind,
                            "payload": payload, "timestamp_us": timestamp_us,
                            "idempotency_key": key, "fingerprint": fingerprint, "response": response,
                        }, changed, response["trades"])
            except Exception as error:
                if not applied and isinstance(error, (core.DuplicateOrderId, core.UnknownOrderId,
                                                       core.OrderNotActive, ValueError, OverflowError)):
                    raise
                # Memory and a database transaction cannot be committed atomically.
                # Block every engine read/write until restart resolves the durable journal.
                self.failed = True
                log.exception("Command failed; engine stopped to avoid serving uncertain state")
                raise Unavailable("command outcome uncertain; restart, then retry with the same Idempotency-Key") from error
            self.last_command += 1
            if key:
                self.idempotency[key] = (fingerprint, response)
            self.publisher.publish(response["trades"])
            return response

    def order(self, id: int):
        with self.lock:
            self._read_available()
            order = self.engine.find_order(id)
            if order is None:
                raise core.UnknownOrderId("unknown order ID")
            return order_dict(order)

    def book(self):
        with self.lock:
            self._read_available()
            return {"symbol": "DEMO", "bids": [level_dict(x) for x in self.engine.bids()],
                    "asks": [level_dict(x) for x in self.engine.asks()]}

    def trades(self, after_id=0, limit=100):
        with self.lock:
            self._read_available()
            return [trade_dict(t) for t in self.engine.trades(after_id, limit)]

    def _read_available(self):
        self._available()
        if self.store is not None:
            try:
                with self.store.begin():
                    self.store.ping()
            except Exception as error:
                self.failed = True
                raise Unavailable("database ownership connection lost; restart required") from error

    def health(self):
        with self.lock:
            database = "disabled"
            if self.store is not None:
                try:
                    with self.store.begin():
                        self.store.ping()
                    database = "ok"
                except Exception:
                    database = "unavailable"
                    self.failed = True
            healthy = not self.failed and self.engine.healthy
            return {"status": "ok" if healthy else "unavailable", "symbol": "DEMO",
                    "mode": "persistent" if self.store is not None else "memory",
                    "database": database, "redis": self.publisher.health()}

    def close(self):
        self.publisher.close()
        if self.store is not None:
            self.store.close()
