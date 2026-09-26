from concurrent.futures import ThreadPoolExecutor

from fastapi.testclient import TestClient
import fakeredis
import pytest
import redis

from api.events import TradePublisher
from api.main import create_app
from api.service import EngineService


@pytest.fixture
def client():
    service = EngineService()
    with TestClient(create_app(service)) as client:
        yield client
    service.engine.check_invariants()
    service.close()


def limit(client, side="BUY", price=100, quantity=5, **extra):
    return client.post("/api/orders", json={"side": side, "type": "LIMIT", "price": price,
                                           "quantity": quantity, **extra})


def test_matching_sweep_queries_and_pagination(client):
    for price, qty in [(99, 20), (100, 30), (101, 70)]:
        assert limit(client, "SELL", price, qty).status_code == 201
    response = limit(client, "BUY", 101, 100)
    assert response.status_code == 201
    assert [t["quantity"] for t in response.json()["trades"]] == [20, 30, 50]
    assert [t["price"] for t in response.json()["trades"]] == [99, 100, 101]
    assert client.get("/api/orders/4").json()["status"] == "FILLED"
    assert client.get("/api/book").json() == {
        "symbol": "DEMO", "bids": [], "asks": [{"price": 101, "total_quantity": 20, "order_count": 1}]}
    assert client.get("/api/book/bids").json() == []
    assert client.get("/api/book/asks").json()[0]["price"] == 101
    assert client.get("/api/trades?after_id=1&limit=1").json()[0]["id"] == 2
    assert client.get("/api/health").json()["status"] == "ok"


@pytest.mark.parametrize("body", [
    {"side": "BUY", "type": "LIMIT", "quantity": 1},
    {"side": "buy", "type": "LIMIT", "price": 1, "quantity": 1},
    *[{"side": "BUY", "type": "LIMIT", "price": 1, "quantity": q} for q in (0, -1, True, 1.5, "5", 2**63)],
    {"side": "SELL", "type": "MARKET", "price": 1, "quantity": 1},
    {"side": "SELL", "type": "MARKET", "price": None, "quantity": 1},
    {"side": "BUY", "type": "LIMIT", "price": 0, "quantity": 1},
    {"side": "BUY", "type": "MARKET", "quantity": 1, "symbol": "XYZ"},
])
def test_invalid_submission(client, body):
    response = client.post("/api/orders", json=body)
    assert response.status_code == 422
    assert response.json()["error"]["code"] == "validation_error"
    assert client.get("/api/book").json()["bids"] == []
    assert limit(client).json()["order"]["id"] == 1


def test_cancel_unknown_duplicate_and_terminal(client):
    assert client.delete("/api/orders/88").status_code == 404
    assert client.get("/api/orders/88").status_code == 404
    assert client.patch("/api/orders/88", json={"price": 10}).status_code == 404
    assert limit(client, id=42).status_code == 201
    assert limit(client, id=42).status_code == 409
    assert client.delete("/api/orders/42").json()["order"]["status"] == "CANCELLED"
    assert client.delete("/api/orders/42").status_code == 409
    assert client.patch("/api/orders/42", json={"price": 10}).status_code == 409
    assert limit(client, id=42).status_code == 409


def test_amendment_priority_and_partial_cancellation(client):
    limit(client, quantity=5)
    limit(client, quantity=5)
    original = client.get("/api/orders/1").json()
    reduced = client.patch("/api/orders/1", json={"remaining_quantity": 3}).json()["order"]
    assert reduced["priority_sequence"] == original["priority_sequence"]
    client.patch("/api/orders/1", json={"remaining_quantity": 6})
    fill = client.post("/api/orders", json={"side": "SELL", "type": "MARKET", "quantity": 7}).json()
    assert [t["maker_order_id"] for t in fill["trades"]] == [2, 1]
    cancelled = client.delete("/api/orders/1").json()["order"]
    assert cancelled["remaining_quantity"] == 4
    assert cancelled["executed_quantity"] == 2


@pytest.mark.parametrize("body", [{}, {"price": None}, {"remaining_quantity": 0}, {"quantity": 5},
                                  {"price": 101, "remaining_quantity": None}])
def test_invalid_amendment(client, body):
    limit(client)
    assert client.patch("/api/orders/1", json=body).status_code == 422
    assert client.get("/api/orders/1").json()["remaining_quantity"] == 5


@pytest.mark.parametrize("path", ["/api/orders/0", "/api/orders/9223372036854775808",
                                   "/api/trades?limit=0", "/api/trades?limit=1001", "/api/trades?after_id=-1"])
def test_path_and_query_bounds(client, path):
    assert client.get(path).status_code == 422


def test_idempotency_key_retry_and_conflict(client):
    body = {"side": "BUY", "type": "LIMIT", "price": 100, "quantity": 5}
    headers = {"Idempotency-Key": "submit-123"}
    first = client.post("/api/orders", json=body, headers=headers)
    assert client.post("/api/orders", json=body, headers=headers).json() == first.json()
    assert client.post("/api/orders", json={**body, "quantity": 6}, headers=headers).status_code == 409
    assert client.delete("/api/orders/1", headers=headers).status_code == 409
    assert client.get("/api/book/bids").json()[0]["order_count"] == 1


def test_concurrent_requests_are_serialized(client):
    with ThreadPoolExecutor(max_workers=8) as executor:
        results = list(executor.map(lambda _: limit(client).json(), range(80)))
    assert sorted(r["order"]["id"] for r in results) == list(range(1, 81))
    assert client.get("/api/book/bids").json()[0]["total_quantity"] == 400


def test_redis_event_and_failed_publication():
    fake = fakeredis.FakeRedis(decode_responses=True)
    publisher = TradePublisher(None, client=fake)
    service = EngineService(publisher=publisher)
    subscriber = fake.pubsub()
    subscriber.subscribe("trades")
    subscriber.get_message(timeout=1)
    with TestClient(create_app(service)) as client:
        limit(client, "SELL")
        limit(client, "BUY")
        import json
        event = json.loads(subscriber.get_message(timeout=1)["data"])
        assert event["trade_id"] == 1 and event["quantity"] == 5
        assert event["symbol"] == "DEMO"
        def fail(*args, **kwargs):
            raise redis.ConnectionError("offline")
        fake.publish = fail
        fake.ping = fail
        limit(client, "SELL")
        response = limit(client, "BUY")
        assert response.status_code == 201
        assert response.json()["trades"][0]["id"] == 2
        assert client.get("/api/health").json()["redis"]["status"] == "degraded"
        assert len(client.get("/api/trades").json()) == 2
    subscriber.close()
    service.close()
