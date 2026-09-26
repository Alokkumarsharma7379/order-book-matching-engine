import json
import os
import time

import pytest
import redis

from api.events import TradePublisher
from api.service import EngineService


@pytest.mark.integration
def test_live_redis_trade_event():
    url = os.getenv("TEST_REDIS_URL")
    if not url:
        pytest.skip("set TEST_REDIS_URL for live Redis verification")
    subscriber_client = redis.Redis.from_url(url, decode_responses=True, socket_timeout=2)
    subscriber = subscriber_client.pubsub()
    subscriber.subscribe("trades")
    assert subscriber.get_message(timeout=2)["type"] == "subscribe"
    service = EngineService(publisher=TradePublisher(url))
    try:
        service.execute("submit", {"side": "SELL", "type": "LIMIT", "price": 100, "quantity": 7})
        result = service.execute("submit", {"side": "BUY", "type": "MARKET", "quantity": 7})
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            message = subscriber.get_message(timeout=0.5)
            if message and message["type"] == "message":
                event = json.loads(message["data"])
                if event.get("executed_at") == result["trades"][0]["executed_at"]:
                    assert event["trade_id"] == 1 and event["quantity"] == 7
                    break
        else:
            pytest.fail("no matching trade event received")
    finally:
        service.close()
        subscriber.close()
        subscriber_client.close()
