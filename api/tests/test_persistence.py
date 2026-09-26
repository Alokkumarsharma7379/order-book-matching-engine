from contextlib import contextmanager
import os
import uuid

from alembic import command
from alembic.config import Config
from fastapi.testclient import TestClient
import pytest
import sqlalchemy as sa

from api.database import Store, commands, metadata, orders, trades
from api.main import create_app
from api.service import EngineService, Unavailable


@pytest.fixture(params=["sqlite", "postgresql"])
def store_factory(request, tmp_path, monkeypatch):
    backend = request.param
    cleanup_engine = None
    if backend == "sqlite":
        url = f"sqlite:///{tmp_path / 'history.db'}"
    else:
        base_url = os.getenv("TEST_DATABASE_URL")
        if not base_url:
            pytest.skip("set TEST_DATABASE_URL for PostgreSQL integration tests")
        schema = "test_" + uuid.uuid4().hex
        cleanup_engine = sa.create_engine(base_url)
        with cleanup_engine.begin() as conn:
            conn.execute(sa.schema.CreateSchema(schema))
        url = sa.engine.make_url(base_url).update_query_dict({"options": f"-csearch_path={schema}"}).render_as_string(hide_password=False)
    monkeypatch.setenv("DATABASE_URL", url)
    command.upgrade(Config("alembic.ini"), "head")
    opened = []
    def factory():
        store = Store(url, allow_sqlite=backend == "sqlite")
        opened.append(store)
        return store
    try:
        yield factory, backend, url
    finally:
        for store in opened:
            store.close()
        if cleanup_engine is not None:
            with cleanup_engine.begin() as conn:
                conn.execute(sa.schema.DropSchema(schema, cascade=True))
            cleanup_engine.dispose()


def submit(service, side="BUY", price=100, quantity=5, key=None):
    return service.execute("submit", {"side": side, "type": "LIMIT", "price": price, "quantity": quantity}, key)


def test_restart_replays_matching_amendment_cancellation_and_idempotency(store_factory):
    factory, _, _ = store_factory
    service = EngineService(factory())
    submit(service, "SELL", quantity=8)
    first = submit(service, "BUY", quantity=3, key="retry-this")
    service.execute("modify", {"id": 1, "price": 101, "remaining_quantity": 7})
    service.execute("cancel", {"id": 1})
    before = [service.order(1), service.order(2), service.book(), service.trades()]
    service.close()
    recovered = EngineService(factory())
    assert [recovered.order(1), recovered.order(2), recovered.book(), recovered.trades()] == before
    assert submit(recovered, "BUY", quantity=3, key="retry-this") == first
    assert recovered.last_command == 4
    assert submit(recovered)["order"]["id"] == 3
    with recovered.store.begin():
        assert recovered.store.connection.scalar(sa.select(sa.func.count()).select_from(trades)) == 1
        assert recovered.store.connection.scalar(sa.select(sa.func.count()).select_from(orders)) == 3


def test_failed_persistence_rolls_back_and_stops_reads_until_restart(store_factory, monkeypatch):
    factory, _, _ = store_factory
    store = factory()
    service = EngineService(store)
    submit(service, "SELL")
    save = store.save
    def fail_after_writes(*args):
        save(*args)
        raise OSError("simulated disk failure before commit")
    monkeypatch.setattr(store, "save", fail_after_writes)
    with TestClient(create_app(service)) as client:
        response = client.post("/api/orders", json={"side": "BUY", "type": "MARKET", "quantity": 5},
                               headers={"Idempotency-Key": "uncertain"})
        assert response.status_code == 503
        assert client.get("/api/book").status_code == 503
        assert client.get("/api/orders/1").status_code == 503
        assert client.get("/api/health").status_code == 503
    service.close()
    recovered = EngineService(factory())
    assert recovered.last_command == 1
    assert recovered.order(1)["remaining_quantity"] == 5
    assert recovered.trades() == []
    assert submit(recovered)["order"]["id"] == 2


def test_lost_commit_acknowledgement_recovers_exactly_one_command(store_factory, monkeypatch):
    factory, _, _ = store_factory
    store = factory()
    service = EngineService(store)
    original_begin = store.begin
    @contextmanager
    def uncertain_commit():
        with original_begin():
            yield
        raise OSError("commit succeeded but acknowledgement was lost")
    monkeypatch.setattr(store, "begin", uncertain_commit)
    with pytest.raises(Unavailable):
        submit(service, key="durable-retry")
    service.close()
    recovered = EngineService(factory())
    response = submit(recovered, key="durable-retry")
    assert response["order"]["id"] == 1
    assert recovered.last_command == 1
    assert recovered.book()["bids"][0]["order_count"] == 1


@pytest.mark.parametrize("corruption", ["journal", "projection"])
def test_recovery_rejects_corruption(store_factory, corruption):
    factory, _, _ = store_factory
    store = factory()
    service = EngineService(store)
    submit(service)
    with store.begin():
        if corruption == "journal":
            store.connection.execute(commands.update().values(response={}))
        else:
            store.connection.execute(orders.update().values(remaining_quantity=4))
    service.close()
    with pytest.raises(RuntimeError, match="disagree"):
        EngineService(factory())


def test_database_constraints_and_no_journal_for_rejected_commands(store_factory):
    factory, _, _ = store_factory
    store = factory()
    service = EngineService(store)
    submit(service)
    with pytest.raises(ValueError):
        submit(service, quantity=0)
    assert service.last_command == 1
    with pytest.raises(sa.exc.IntegrityError):
        with store.begin():
            store.connection.execute(orders.update().values(price=None))
    assert service.health()["status"] == "ok"


def test_postgresql_rejects_a_second_writer_and_lost_ownership(store_factory):
    factory, backend, _ = store_factory
    if backend != "postgresql":
        pytest.skip("PostgreSQL session advisory locks")
    first = factory()
    service = EngineService(first)
    with pytest.raises(RuntimeError, match="another process"):
        factory()
    first.connection.invalidate()
    with pytest.raises(Unavailable):
        service.book()
    with pytest.raises(Unavailable):
        submit(service)
    assert service.failed


def test_migration_matches_models(store_factory):
    factory, _, _ = store_factory
    store = factory()
    inspector = sa.inspect(store.connection)
    for table in metadata.tables.values():
        assert {col["name"] for col in inspector.get_columns(table.name)} == set(table.columns.keys())
    store.connection.rollback()
