# Phases 9–12: API, PostgreSQL, Redis, and tests

## Request boundary

`api/main.py` defines the application factory, lifespan, routes, and centralized
errors. `api/schemas.py` validates commands and describes response schemas.
`api/service.py` owns one engine and serializes all commands/queries with an RLock.
Synchronous endpoints run in FastAPI's worker thread pool; the lock orders access.
Arrival at the lock, not client network send time, determines command order.
Lifespan manages ownership and cleanup using
[FastAPI's documented mechanism](https://fastapi.tiangolo.com/advanced/events/).

The API validates integer bounds and excludes unknown fields. Market submissions
must omit price, including null price. PATCH accepts a price and/or remaining
quantity and rejects empty or null amendments. A completed mutation returns an
order snapshot plus its trades; retries with the same key return the original
snapshot. Trade pagination uses an exclusive `after_id` cursor.

Without PostgreSQL, `MEMORY_MODE=1` must be explicitly selected. No automatically
degraded memory mode exists. Configuration is read at startup, not module import,
so tests can construct isolated services. Readiness returns 503 after a service
failure; Redis degradation alone still returns 200 because matching is available.

## Durable history and restart

`api/database.py` uses SQLAlchemy Core tables and one dedicated connection. Explicit
transactions fit this small write-oriented adapter without ORM identity tracking.
The initial Alembic migration is a frozen schema, independent of future model edits.

| Table | Stored information |
| --- | --- |
| orders | Current state of every active/terminal order, accounting, sequences, timestamps |
| trades | Immutable execution records with maker/taker foreign keys |
| commands | Accepted command order, version, request, timestamp, retry key, fingerprint, response |

Matching never queries SQL for an opposite order or a price comparison. After C++
execution, Python writes all affected orders, new trades, and the command record
in one database transaction. A command producing F trades updates at most F + 1
orders; SQL round trips and commits add latency outside the core benchmark.

Startup replays the journal into a fresh engine, compares each response, verifies
invariants, then compares final SQL projections against memory. This also restores
priority, IDs, and the retry cache. Replay is O(journal commands + matching work +
retained records/trades); the current implementation loads the journal and comparison
snapshots into memory. No checkpoint or schema/behavior-version upgrade replay exists.

The connection holds a [PostgreSQL session advisory lock](https://www.postgresql.org/docs/current/explicit-locking.html#ADVISORY-LOCKS)
for the DEMO writer. A second process fails startup. The service checks the same
connection before mutations and reads. A broken ownership connection is never
silently replaced: restart is required. Run one Uvicorn worker and apply migrations
with one deployment process. The database account must be allowed to own this lock
and read/write the application's tables.

## Failure contract

| Failure | Behavior |
| --- | --- |
| Invalid/duplicate/inactive command | Reject; no accepted command record or counter consumption |
| SQL unavailable before matching | 503; service stops |
| Write/commit failure after memory changed | 503; all engine queries/mutations stop |
| Commit succeeded but acknowledgement was lost | Stop; replay on restart includes the command |
| Same retry key and command after restart | Return recorded response without executing again |
| Retry key reused for a different command | 409 |
| Corrupt journal or inconsistent projections | Refuse startup |
| Redis fails after commit | Command remains successful; warning/degraded publication status |

The service cannot atomically commit C++ memory and PostgreSQL. Fail-stop plus
deterministic replay makes the boundary explicit. Returning an ordinary rejection
after an uncertain commit would invite unsafe retries; callers should retain and
reuse an idempotency key. Without a key, exactly-once retry behavior is not provided.

## Live events

`api/events.py` publishes JSON on Redis channel `trades` after a command commits.
Timeouts are bounded and retries disabled. Publication is outside matching but
inside the service lock to preserve publish order for this process. This means
slow Redis can still add API latency. The publisher logs failures and counts events
whose publication failed or was skipped after a failure in the batch.

Redis [Pub/Sub is at-most-once and not durable](https://redis.io/docs/latest/develop/pubsub/).
A disconnected subscriber misses events. A process crash between SQL commit and
publish also loses the notification. Restart/retry does not republish old events.
The SQL-backed recovered trade history is the reconciliation source; there is no
outbox or subscriber acknowledgement. `failed_events` is a local observed count,
not a count of all missed subscriber deliveries. Redis is used for live consumers,
not for book state or a redundant cache.

## Run and test

```powershell
$env:DATABASE_URL = 'postgresql+psycopg://orderbook:orderbook_dev@127.0.0.1:5432/orderbook'
& .\.venv\Scripts\python.exe -m alembic upgrade head
& .\.venv\Scripts\python.exe -m uvicorn api.main:app --host 127.0.0.1 --workers 1
```

In another shell:

```powershell
& .\.venv\Scripts\python.exe scripts/smoke_api.py
$env:TEST_DATABASE_URL = 'postgresql+psycopg://orderbook:orderbook_dev@127.0.0.1:5432/orderbook_test'
# Optional: requires a running Redis server.
$env:TEST_REDIS_URL = 'redis://127.0.0.1:6379/0'
& .\.venv\Scripts\python.exe -m pytest -q
```

`smoke_api.py` prints `Live HTTP smoke test passed`. It creates a real order and
cancels any remainder; use a development service. PostgreSQL tests allocate their
own random schemas, apply the actual migration, and remove those schemas afterward.
SQLite fixtures cover the same transaction/replay paths but cannot validate
PostgreSQL locking or types. Live PostgreSQL verification was performed locally.

Tests include strict validation, response codes, complete sweeps, amendment
priority, cancellation, pagination, concurrent requests, retry-key conflicts,
real database rollback/restart, commit acknowledgement loss, corruption detection,
writer ownership, and simulated Redis publication/disconnection. The optional
live Redis test is distinct from fakeredis coverage; it was not run locally.

Interview topics: What is an uncertain commit? Why are order/trade tables alone
insufficient to reconstruct priority after amendments? Why can a successful
command have no delivered Pub/Sub event? Where does serialization reduce throughput?
