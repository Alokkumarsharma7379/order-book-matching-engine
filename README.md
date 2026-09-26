# Order Book & Matching Engine

An **exchange-style educational matching engine** with a C++20 core, pybind11
bindings, a FastAPI service, PostgreSQL history and recovery, and Redis trade
notifications. The purpose is to make price-time matching, iterator-safe storage,
failure handling, and backend integration understandable and testable.

The current implementation supports one instrument, `DEMO`. Prices are integer
cents: `10100` means `101.00`. Quantities are positive integer units. No floating
point is used for prices, quantities, or execution accounting.

## Architecture

```mermaid
flowchart LR
    Client --> API[FastAPI validation]
    API --> Service[Serialized command service]
    Service --> Binding[pybind11]
    Binding --> Engine[C++ matching engine]
    Engine --> Book[Maps, FIFO lists, ID indexes]
    Service --> PG[(PostgreSQL journal and projections)]
    PG --> Replay[Startup replay and verification]
    Replay --> Engine
    Service --> Redis[Redis Pub/Sub: trades]
    Redis --> Subscribers[Live subscribers]
```

The core has no database, HTTP, or Redis dependency. One Python lock covers an
entire command, its database transaction, and publication. The C++ binding keeps
the GIL. Run **one Uvicorn worker**; a PostgreSQL session advisory lock rejects a
second engine writer. In explicit memory mode, no cross-process guard exists.

## Matching rules and lifecycle

- Bids prefer higher prices; asks prefer lower prices. At one price, earlier
  priority sequence wins. Wall-clock timestamps do not determine priority.
- A buy crosses when its limit is at least the best ask; a sell crosses when its
  limit is at most the best bid. Trades execute at the resting maker's price.
- A limit remainder rests. A market order consumes available liquidity and
  cancels its unexecuted remainder; it never rests.
- Price changes and quantity increases lose FIFO priority. A same-price decrease
  retains priority. An identical amendment changes nothing in the core.
- `remaining_quantity` in PATCH means the new **unexecuted** quantity. Original
  quantity remains immutable; `quantity_adjustment` records net amendments.
- Filled/cancelled records remain queryable. Their IDs cannot be reused.

Lifecycle: `NEW -> PARTIALLY_FILLED -> FILLED`, or cancellation while active.
An unfilled market order becomes `CANCELLED` immediately. A cancelled partial
order retains both its executed quantity and its cancelled remaining quantity.

For a buy of 100 at 101 against asks of 20 at 99, 30 at 100, and 70 at 101, the
engine emits fills of **20, 30, 50** at those maker prices. The buy fills completely
and 20 remains at ask 101. This scenario is covered on both sides by C++ tests.

## Data structures and complexity

Descending/ascending `std::map` containers hold bid/ask levels. Each level has a
`std::list<OrderId>` FIFO and a cached quantity total. Hash maps own order records
and locate active orders by side, price, and list iterator. Cancellation needs no
full-book scan. Orders and query results cross the public boundary as copies.

For L levels, F fills, H retained order records, and T trades:

| Core operation | Expected time | Additional/output space |
| --- | --- | --- |
| Rest a limit order | O(log L), amortized | O(1) |
| Cancel / reduce at same price | O(log L) | O(1) |
| Match / crossing amendment | O(log L + F), amortized | O(F) |
| ID lookup / best bid or ask | O(1) | O(1) |
| Aggregated book snapshot | O(L) | O(L) |
| Trade page of K records | O(log T + K) | O(K) |

Hash bounds are expected, not worst-case. History vector growth occasionally
copies prior trades. Memory is O(H + active orders + L + T); history is currently
unbounded. See [storage](docs/order-book.md), [matching](docs/matching.md), and
[amendment](docs/order-management.md) explanations for iterator and rollback details.

## Build and run on Windows

From x64 PowerShell with Python 3.14 installed:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
& .\scripts\setup-python-windows.ps1
$env:MEMORY_MODE = '1'
& .\.venv\Scripts\python.exe -m uvicorn api.main:app --host 127.0.0.1 --port 8000 --workers 1
```

Open `http://127.0.0.1:8000/docs` for the interactive OpenAPI interface. Memory mode
is an explicit demo mode: stopping the process loses its state. No silent fallback
occurs when a configured database fails. [Bindings](docs/python-bindings.md) covers
the locally verified Windows compiler and wheel packaging.

For persistent native operation, create a PostgreSQL database, then in the same shell:

```powershell
Remove-Item Env:MEMORY_MODE -ErrorAction SilentlyContinue
$env:DATABASE_URL = 'postgresql+psycopg://orderbook:orderbook_dev@127.0.0.1:5432/orderbook'
$env:REDIS_URL = 'redis://127.0.0.1:6379/0'
& .\.venv\Scripts\python.exe -m alembic upgrade head
& .\.venv\Scripts\python.exe -m uvicorn api.main:app --host 127.0.0.1 --port 8000 --workers 1
```

Use credentials for your own database. Redis is optional for native operation;
omit `REDIS_URL` to disable publication.

On Linux with a C++20 compiler, CMake, Ninja, and Python development headers:

```sh
python -m venv .venv
. .venv/bin/activate
pip install -r requirements.lock -r requirements-build.txt
pip install . --no-build-isolation --no-deps
MEMORY_MODE=1 uvicorn api.main:app --host 127.0.0.1 --port 8000 --workers 1
```

## Docker Compose

With Docker Engine/Desktop and Compose available:

```sh
docker compose up --build
```

This defines an API image that compiles the C++ extension, PostgreSQL with a named
data volume, and Redis. The API runs migrations before serving, uses one worker,
and binds port 8000 to the host loopback interface. PostgreSQL and Redis ports
are private to the Compose network. Stop with `docker compose down`; the database
volume remains. See [deployment](docs/deployment.md).

**Verification limit:** this development machine has no Docker/WSL runtime.
Compose YAML was checked locally, but the container build and live Redis test
have not run here. The included CI workflow runs those checks on Linux when the
repository is hosted and CI is executed; adding the workflow does not mean it passed.

## REST API

| Method and path | Behavior |
| --- | --- |
| `POST /api/orders` | Submit a limit/market order; 201 with order and generated trades |
| `GET /api/orders/{id}` | Retrieve an active or terminal order |
| `DELETE /api/orders/{id}` | Cancel an active order |
| `PATCH /api/orders/{id}` | Change price and/or remaining quantity; returns any resulting trades |
| `GET /api/book` | Aggregated bids and asks for DEMO |
| `GET /api/book/bids`, `/api/book/asks` | One side of the book |
| `GET /api/trades?after_id=0&limit=100` | Trade history; limit 1–1000 |
| `GET /api/health` | Engine/database readiness and Redis status |

Malformed commands receive 422, unknown IDs 404, duplicate/inactive orders or
conflicting retry keys 409, and an unavailable engine 503. Errors use an `error`
object with `code` and `message`. Prices and quantities reject floats, strings,
booleans, nonpositive values, and integers exceeding signed 64-bit range.

PowerShell examples (use a fresh demo for predictable order IDs):

```powershell
Invoke-RestMethod http://127.0.0.1:8000/api/orders -Method Post -ContentType application/json `
  -Headers @{'Idempotency-Key'='ask-1'} `
  -Body '{"side":"SELL","type":"LIMIT","price":10100,"quantity":50}'
Invoke-RestMethod http://127.0.0.1:8000/api/orders -Method Post -ContentType application/json `
  -Headers @{'Idempotency-Key'='buy-1'} `
  -Body '{"side":"BUY","type":"MARKET","quantity":25}'
Invoke-RestMethod http://127.0.0.1:8000/api/orders/1 -Method Patch -ContentType application/json `
  -Body '{"remaining_quantity":10}'
Invoke-RestMethod http://127.0.0.1:8000/api/book
Invoke-RestMethod http://127.0.0.1:8000/api/trades
Invoke-RestMethod http://127.0.0.1:8000/api/orders/1 -Method Delete
```

Use a unique `Idempotency-Key` on each mutation and reuse it only for retries of
that same command. Retry results reproduce the original response, which may
differ from the order's current state. Query the order for its current state.
Keys survive restart in persistent mode. Without a key, retrying after a lost
response can submit another order. JSON clients must preserve 64-bit integers;
JavaScript `Number` cannot exactly represent all supported IDs/quantities.

## PostgreSQL and Redis

PostgreSQL stores orders, trades, and an ordered command journal in one transaction
per accepted API command. All affected maker orders are updated. The in-memory
book serves matching comparisons without SQL reads. On startup, accepted commands
are replayed, results compared, invariants checked, and SQL projections verified.

Memory and SQL cannot commit atomically. A persistence failure stops the service;
reads and writes return 503 until restart reconstructs committed state. An uncertain
commit is resolved by replay and a retry with the same idempotency key. This trades
availability for consistency. See [service design](docs/service.md).

Redis publishes committed executions on `trades`. Notifications include symbol,
trade ID, maker/taker IDs, integer price/quantity, sequence, and UTC timestamp. Redis
failure leaves the command successful, records a warning/degraded status, and may
lose notifications. Pub/Sub is not durable; use the trade API to reconcile gaps.
There is no transactional outbox or delivery guarantee.

## Tests and verification

```powershell
& .\scripts\setup-windows.ps1
cmake --preset debug
cmake --build --preset debug --parallel 2
ctest --preset debug
cmake --preset sanitizers
cmake --build --preset sanitizers --parallel 2
ctest --preset sanitizers
& .\.venv\Scripts\python.exe -m pytest -q
```

Set `TEST_DATABASE_URL` to a disposable PostgreSQL database to run integration
tests. Each fixture creates and removes its own uniquely named schema. Set
`TEST_REDIS_URL` to run live Pub/Sub verification. [Testing](docs/testing.md) describes
the C++ suite; [service design](docs/service.md) explains backend verification.

Locally verified: 51 CTest entries in Debug, Release, and ASan/UBSan; Python binding,
API, PostgreSQL, rollback/recovery, concurrency, and simulated Redis-failure tests;
and real HTTP requests to a persistent Uvicorn process. Exact final results and
remaining limits are recorded in [the review](docs/review.md).

## Benchmarks

```powershell
cmake --preset release -DORDERBOOK_BUILD_BENCHMARKS=ON
cmake --build --preset release --parallel 2
& .\.venv\Scripts\python.exe scripts/benchmark.py `
  --executable build/release/orderbook_benchmark.exe --commands 250000 --trials 7 --warmup 10000
```

The runner retains raw CSV, compiler/platform/source hashes, and a summary.
[Measured results](benchmark-results/windows-clang21-verified/summary.md) come from
an actual local run. For example, the frequent-crossing workload had a median
**2,491,203.6 submitted orders/s across seven trials**. This measures the standalone
core with its measurement harness, not HTTP throughput or production exchange
capacity. [Methodology](docs/benchmarking.md) explains warmup, latency percentiles,
timer granularity, workload state, and interpretation limits.

## Limitations and future work

This is not a production trading system. It has no authentication, account/risk
checks, self-trade prevention, fees, cancel/replace FIX protocol, or multi-symbol
router. Book snapshots, retained history, retry cache, and replay cost grow without
bounds. SQL and Redis work run under the service lock, limiting API throughput.
No disk snapshot/checkpoint, replication, failover controller, outbox, or retry-key
retention policy exists. Dependency versions are pinned, but container tags/system
packages are not digest-locked. The native Windows wheel was tested with the local
LLVM-MinGW toolchain, not every Windows compiler/CPython combination.

Useful next steps are bounded history/checkpoints, durable event delivery, admission
limits, stronger deployment verification, and per-symbol engine partitions. A
producer/consumer queue can sequence commands to a dedicated engine thread; adding
locks inside the matching loop would not by itself make matching faster. Symbols
can be partitioned across independent engines; ordering within a symbol still needs
a single authority.

## Learning and interviews

Start with [the interview exercises](docs/interview.md), explain your reasoning
before reading code, and use the tests to check your answers. Topics include
ordered vs unordered containers, FIFO, iterator validity, maker/taker pricing,
failure boundaries, Pub/Sub, and why a laptop benchmark is not a capacity guarantee.
[Resume wording](docs/resume.md) uses only implemented behavior and measured results.

Complete phase-by-phase file inventories and build commands are in
[the implementation record](docs/phases-8-17.md).
