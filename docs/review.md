# Phase 16: engineering review

## Changes made during review

- A Linux-style default build generator was not selected automatically for the
  local Windows wheel; the setup script now explicitly selects Ninja.
- pybind11's optional strip command failed to quote the `&` in this workspace's
  path. Optional extras and wheel stripping are disabled; Release optimization remains.
- A PostgreSQL CHECK involving a nullable limit price could evaluate to SQL NULL
  and pass. The schema now explicitly requires `price IS NOT NULL` for limit orders,
  with a regression test against the real database and SQLite.
- Expected engine validation failures and failures after engine mutation must not
  share a catch-and-continue path. The service tracks successful application and
  stops if persistence/commit fails afterward.
- Queries and cached retry responses now validate the ownership connection, so a
  disconnected former writer does not keep serving its old book.
- Startup checks final order/trade SQL projections as well as command responses,
  rejecting inconsistent persisted state.
- Redis client retries are disabled explicitly, so failure latency does not grow
  through the library's retry policy. Publication failures are logged and visible.
- Benchmark mean formatting returned zero for long double on this Windows toolchain.
  The measured average now uses double, and the runner validates output before
  reporting. The initial artifact is marked invalid; a complete rerun is retained.

## Verification performed

- Final configure/build/CTest runs passed all 51 entries in Debug, Release, and
  ASan/UBSan, with no sanitizer diagnostics.
- CPython 3.14.3 imported the native wheel without compiler directories on PATH.
- Python suite with the isolated PostgreSQL server: **46 passed, 2 skipped**.
  One skip is the SQLite parameter of a PostgreSQL-only advisory-lock check; the
  other is live Redis, whose server is unavailable locally.
- Tests applied the actual Alembic migration in isolated schemas, verified
  projections/replay, rolled back failed writes, simulated lost commit acknowledgement,
  rejected a second writer, and detected corrupt history.
- A real Uvicorn process backed by PostgreSQL passed the HTTP smoke script.
- fakeredis covered event serialization and publication failure behavior.
- All 35 benchmark trials completed with invariant checks; raw CSV and metadata
  are retained. Compose YAML parsed and configuration assertions passed.
- `pip check` reported no broken dependencies. Starlette emits one upstream
  deprecation warning for its currently supported httpx test-client fallback.

## Remaining weaknesses and practical limits

| Area | Finding and implication |
| --- | --- |
| Container/live integration | No Docker/WSL or live Redis server locally. Container build, Linux runtime, and hosted CI are unverified. |
| Atomicity | C++ memory and SQL are not one transaction. Failure requires restart; there is no online rollback/recovery controller. |
| Redis delivery | No outbox. Crashes or disconnections can lose events even after successful SQL commit. |
| Availability | Database connectivity is required for persistent reads/writes. Single writer has no automatic failover. |
| Resource bounds | Orders, trades, retry cache, startup journal, and full book snapshots can grow without limits. |
| Replay/versioning | Replay checks version 1 only; behavior upgrades need a migration/replay compatibility strategy. |
| Throughput | SQL writes and publication run under the service lock. Core benchmark results cannot predict API capacity. |
| Input/admission | Schema validates values, but there is no authentication, authorization, rate limit, risk limit, or request-size proxy policy. |
| Database operations | No production backup, retention, checkpoint, transaction timeout policy, or concurrent migration coordinator. |
| Concurrency | One symbol and worker only. The standalone core is not thread-safe. Memory mode cannot prevent multiple independent processes. |
| Numeric boundaries | Signed 64-bit engine bounds are tested. JavaScript clients must avoid precision loss when handling large JSON integers. |
| Iterator safety | Lists and location indexes are checked by invariants, generated workloads, and sanitizers; this is strong test evidence, not a formal proof. |
| Failure coverage | No exhaustive fuzzing, forced private trade/sequence counter exhaustion, OS-level crash matrix, or over-aligned allocator injection. |
| Reproducibility | Python versions are pinned; container/system package digests are not. The tested Windows wheel is one toolchain/ABI combination. |
| Benchmark interpretation | Short local trials, timer quantization, uncontrolled scheduling/frequency, and synthetic distributions limit generalization. |

Priority improvements are durable event delivery, bounded memory/checkpoints,
container/live Redis verification, resource admission limits, and tested operational
recovery. A more complex matching data structure is not justified by these results
alone. No production-readiness claim is made.
