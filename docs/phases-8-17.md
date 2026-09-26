# Implementation record: phases 8–17

The user authorized completing all remaining phases without confirmation between
them. This record preserves the phase boundaries, source inventory, reasoning,
commands, expected results, and honest verification status. The complete code is
in the repository and bundled in `build/phases-8-17-source.md`.

## Phase 8: pybind11

Created `engine/src/bindings.cpp`, `python/orderbook/__init__.py`, `pyproject.toml`,
`requirements.lock`, `scripts/setup-python-windows.ps1`, `tests/test_bindings.py`,
and `docs/python-bindings.md`; modified `CMakeLists.txt`.

Read-only snapshots keep ownership in C++; integer microsecond timestamps avoid
timezone ambiguity. A CMake-built wheel reuses the core. The Windows build carries
its C++ runtime DLLs. See [binding decisions](python-bindings.md).

```powershell
& .\scripts\setup-python-windows.ps1
& .\.venv\Scripts\python.exe -m pytest tests/test_bindings.py -q
```

Verified: wheel build/import and three passing binding tests.

## Phase 9: REST API

Created `api/__init__.py`, `api/main.py`, `api/schemas.py`, `api/service.py`, and
`scripts/smoke_api.py`. One lock serializes commands and queries. Typed responses,
strict integer input, bounded trade pagination, and centralized errors form the
HTTP contract. [Service design](service.md) explains alternatives and costs.

```powershell
$env:MEMORY_MODE = '1'
& .\.venv\Scripts\python.exe -m uvicorn api.main:app --host 127.0.0.1 --workers 1
# From another shell:
& .\.venv\Scripts\python.exe scripts/smoke_api.py
```

Expected: `/api/health` reports `ok`; smoke prints `Live HTTP smoke test passed`.

## Phase 10: PostgreSQL

Created `api/database.py`, `alembic.ini`, `migrations/env.py`,
`migrations/script.py.mako`, and `migrations/versions/0001_initial_command_journal_orders_and_.py`;
extended the service with transactions, journal replay, ownership, and retry keys.

SQLAlchemy Core keeps the adapter small. Order/trade projections are useful for
history, while the journal preserves accepted command order and amendment priority.
Failed or uncertain persistence stops the service instead of serving divergent state.

```powershell
$env:DATABASE_URL = 'postgresql+psycopg://orderbook:orderbook_dev@127.0.0.1:5432/orderbook'
& .\.venv\Scripts\python.exe -m alembic upgrade head
```

Expected: schema reaches revision `0001`. Verified with a separate local PostgreSQL
18 cluster in `.tools/pg-test`, listening only on 127.0.0.1:55432, and isolated test
schemas. The pre-existing PostgreSQL service/data were not modified.

## Phase 11: Redis

Created `api/events.py`; extended service publication and health reporting.
Redis publishes committed trades for live subscribers. It is deliberately outside
the core and does not decide command acceptance. No durable-delivery claim is made.

```powershell
$env:REDIS_URL = 'redis://127.0.0.1:6379/0'
# With Redis CLI installed, subscribe before submitting crossing orders:
redis-cli SUBSCRIBE trades
```

Expected: JSON trade events. Simulated publication/failure behavior passed; live
Redis execution remains unverified locally because no server/runtime is available.

## Phase 12: API and integration tests

Created `api/tests/test_api.py`, `api/tests/test_persistence.py`, and
`api/tests/test_live_redis.py`.

```powershell
$env:TEST_DATABASE_URL = 'postgresql+psycopg://orderbook:orderbook_dev@127.0.0.1:5432/orderbook_test'
& .\.venv\Scripts\python.exe -m pytest -q
```

Verified local result with real PostgreSQL and no Redis server: **46 passed,
2 skipped**. Skips are the SQLite parameter of a PostgreSQL-only lock check and
the optional live Redis test. Without `TEST_DATABASE_URL`, PostgreSQL cases skip
as well. A single upstream httpx test-client deprecation warning remains.
A separate persistent Uvicorn process passed actual HTTP requests.

## Phase 13: Docker and Compose

Created `Dockerfile`, `docker-compose.yml`, `.dockerignore`, `.env.example`,
`api/run.py`, `.github/workflows/ci.yml`, and `docs/deployment.md`.

```sh
docker compose config --quiet
docker compose up --build
```

Expected: three services, migrated/recovered API on port 8000, health checks.
Verified only YAML parsing and configuration assertions here. Docker, live Redis,
and hosted Linux CI are not locally executed; [deployment notes](deployment.md)
describe the exact remaining verification.

## Phase 14: benchmark

Created `engine/benchmarks/benchmark.cpp`, `scripts/benchmark.py`,
`docs/benchmarking.md`, and measured artifacts under
`benchmark-results/windows-clang21-verified/`; added the optional CMake target.

```powershell
cmake --preset release -DORDERBOOK_BUILD_BENCHMARKS=ON
cmake --build --preset release --parallel 2
& .\.venv\Scripts\python.exe scripts/benchmark.py `
  --executable build/release/orderbook_benchmark.exe --commands 250000 --trials 7 --warmup 10000
```

Verified: 35 measured trials over five workloads, invariant checks, raw CSV,
latency distributions, compiler/platform/source metadata. Output varies by run;
the retained results are measurements, not hard-coded benchmark output. The first
development run is marked invalid because of a mean formatting issue.

## Phase 15: README

Created `README.md` and `docs/service.md`; updated prior build, matching, and
amendment guides to point to the completed adapters. Architecture, complexity,
lifecycle, API examples, data boundaries, commands, limitations, and future work
are documented. Examples use local development credentials and one worker.

## Phase 16: review and fixes

Created `docs/review.md`; tightened schema null handling, failure classification,
ownership checks on queries, recovery projection verification, bounded Redis retries,
and benchmark output validation. The core matching implementation was unchanged.

Final CMake configure/build/CTest runs passed **51/51** in Debug, Release, and
ASan/UBSan. The Python suite and real HTTP smoke test passed as recorded above.
The review retains actual limits; it does not turn unavailable environment checks
into passing results.

## Phase 17: interview preparation

Created `docs/interview.md` and `docs/resume.md`. The exercises require tracing this
implementation, explaining alternatives and costs, and defending actual test and
benchmark evidence. Resume wording distinguishes the core benchmark from API
capacity and simulated Redis coverage from live verification.

Your next learning task is to explain why this cancellation path is O(log L)
despite expected O(1) ID lookup, then trace how a quantity increase changes FIFO.
Those are practice questions, not permission gates for further implementation.
