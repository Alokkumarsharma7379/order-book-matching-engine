from contextlib import asynccontextmanager
import os
from typing import Annotated

from fastapi import FastAPI, Header, Path, Query, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
import orderbook as core

from .database import Store
from .events import TradePublisher
from .schemas import BookView, ExecutionView, LevelView, MAX_INT, ModifyOrder, OrderView, SubmitOrder, TradeView
from .service import EngineService, IdempotencyConflict, Unavailable

OrderId = Annotated[int, Path(gt=0, le=MAX_INT)]
Key = Annotated[str | None, Header(alias="Idempotency-Key", min_length=1, max_length=128, pattern=r"^[A-Za-z0-9._:-]+$")]


def create_app(service: EngineService | None = None) -> FastAPI:
    @asynccontextmanager
    async def lifespan(app):
        owned = service is None
        if owned:
            url = os.getenv("DATABASE_URL")
            if not url and os.getenv("MEMORY_MODE") != "1":
                raise RuntimeError("set DATABASE_URL or explicitly set MEMORY_MODE=1")
            store = Store(url) if url else None
            try:
                app.state.service = EngineService(store, TradePublisher(os.getenv("REDIS_URL")))
            except BaseException:
                if store is not None:
                    store.close()
                raise
        else:
            app.state.service = service
        try:
            yield
        finally:
            if owned:
                app.state.service.close()

    app = FastAPI(title="Order Book & Matching Engine", version="0.1.0", lifespan=lifespan,
                  description="An exchange-style educational matching engine for DEMO. Prices are integer cents.")

    def error_handler(status, code):
        async def handle(request, error):
            return JSONResponse(status_code=status, content={"error": {"code": code, "message": str(error)}})
        return handle

    for exception, status, code in [
        (core.UnknownOrderId, 404, "order_not_found"),
        (core.DuplicateOrderId, 409, "duplicate_order_id"),
        (core.OrderNotActive, 409, "order_inactive"),
        (IdempotencyConflict, 409, "idempotency_conflict"),
        (Unavailable, 503, "engine_unavailable"),
        (OverflowError, 422, "numeric_overflow"),
        (ValueError, 422, "invalid_command"),
    ]:
        app.add_exception_handler(exception, error_handler(status, code))

    @app.exception_handler(RequestValidationError)
    async def validation_error(request, error):
        return JSONResponse(status_code=422, content={"error": {
            "code": "validation_error", "message": "request validation failed",
            "details": [{"location": list(e["loc"]), "message": e["msg"]} for e in error.errors()],
        }})

    @app.post("/api/orders", response_model=ExecutionView, status_code=201)
    def submit(body: SubmitOrder, request: Request, key: Key = None):
        return request.app.state.service.execute("submit", body.model_dump(exclude_none=True), key)

    @app.get("/api/orders/{order_id}", response_model=OrderView)
    def order(order_id: OrderId, request: Request):
        return request.app.state.service.order(order_id)

    @app.delete("/api/orders/{order_id}", response_model=ExecutionView)
    def cancel(order_id: OrderId, request: Request, key: Key = None):
        return request.app.state.service.execute("cancel", {"id": order_id}, key)

    @app.patch("/api/orders/{order_id}", response_model=ExecutionView)
    def modify(order_id: OrderId, body: ModifyOrder, request: Request, key: Key = None):
        return request.app.state.service.execute("modify", {"id": order_id, **body.model_dump(exclude_none=True)}, key)

    @app.get("/api/book", response_model=BookView)
    def book(request: Request):
        return request.app.state.service.book()

    @app.get("/api/book/bids", response_model=list[LevelView])
    def bids(request: Request):
        return request.app.state.service.book()["bids"]

    @app.get("/api/book/asks", response_model=list[LevelView])
    def asks(request: Request):
        return request.app.state.service.book()["asks"]

    @app.get("/api/trades", response_model=list[TradeView])
    def trades(request: Request, after_id: Annotated[int, Query(ge=0, le=MAX_INT)] = 0,
               limit: Annotated[int, Query(ge=1, le=1000)] = 100):
        return request.app.state.service.trades(after_id, limit)

    @app.get("/api/health")
    def health(request: Request):
        result = request.app.state.service.health()
        return JSONResponse(result, status_code=200 if result["status"] == "ok" else 503)

    return app


app = create_app()
