"""Exercise a running API with unique IDs and retry keys; leaves historical records."""
import argparse
import json
import urllib.request
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8000")
    args = parser.parse_args()
    def call(method, path, body=None):
        request = urllib.request.Request(args.url + path, method=method,
            data=json.dumps(body).encode() if body is not None else None,
            headers={"Content-Type": "application/json", "Idempotency-Key": str(uuid.uuid4())})
        with urllib.request.urlopen(request, timeout=10) as response:
            return json.load(response)
    assert call("GET", "/api/health")["status"] == "ok"
    order = call("POST", "/api/orders", {"side": "BUY", "type": "LIMIT", "price": 1, "quantity": 1})["order"]
    fetched = call("GET", f'/api/orders/{order["id"]}')
    assert fetched["id"] == order["id"]
    if fetched["status"] in ("NEW", "PARTIALLY_FILLED"):
        assert call("DELETE", f'/api/orders/{order["id"]}')["order"]["status"] == "CANCELLED"
    assert call("GET", "/api/book")["symbol"] == "DEMO"
    assert isinstance(call("GET", "/api/trades"), list)
    print("Live HTTP smoke test passed")


if __name__ == "__main__":
    main()
