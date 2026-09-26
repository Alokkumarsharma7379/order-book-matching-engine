import json
import logging

import redis
from redis.backoff import NoBackoff
from redis.retry import Retry

log = logging.getLogger(__name__)


class TradePublisher:
    """Best-effort live notifications; PostgreSQL remains the historical record."""

    def __init__(self, url: str | None, *, client=None):
        self.client = client if client is not None else (
            redis.Redis.from_url(url, socket_timeout=0.2, socket_connect_timeout=0.2,
                                 retry=Retry(NoBackoff(), 0), decode_responses=True) if url else None
        )
        self.status = "disabled" if self.client is None else "unknown"
        self.failed_events = 0

    def publish(self, trades: list[dict]):
        if self.client is None:
            return
        for index, trade in enumerate(trades):
            try:
                self.client.publish("trades", json.dumps({"symbol": "DEMO", "trade_id": trade["id"], **trade}))
                self.status = "ok"
            except redis.RedisError:
                self.status = "degraded"
                self.failed_events += len(trades) - index
                log.warning("Redis publication failed; committed trades remain queryable", exc_info=True)
                break

    def health(self):
        if self.client is not None:
            try:
                self.client.ping()
                self.status = "ok"
            except redis.RedisError:
                self.status = "degraded"
        return {"status": self.status, "failed_events": self.failed_events}

    def close(self):
        if self.client is not None:
            self.client.close()
