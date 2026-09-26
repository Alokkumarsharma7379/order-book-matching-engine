# Phase 13: Docker and deployment

`Dockerfile` builds a native Linux wheel in a compiler-equipped stage, then installs
the wheel and runtime dependencies into a slim Python image. The runtime uses a
non-root user. `api/run.py` applies Alembic migrations and starts one Uvicorn worker.
`.dockerignore` excludes local environments, binaries, secrets, and benchmark output.

`docker-compose.yml` defines API, PostgreSQL, and Redis services. PostgreSQL readiness
gates API startup through [Compose health dependencies](https://docs.docker.com/compose/how-tos/startup-order/).
Redis has a health check but only a started dependency, because publication may
degrade without making matching unavailable. The API health endpoint checks the
database ownership connection; Redis failure does not trigger API restart.

The PostgreSQL 18 image mounts its data volume at `/var/lib/postgresql`, following
the [official image's version-specific layout](https://hub.docker.com/_/postgres).
Only API port 8000 is published, bound to loopback. The example credentials are
for local development. The service has no authentication and should remain local.

## Commands

```sh
docker compose config --quiet
docker compose up --build
```

Expected: PostgreSQL becomes healthy, migrations complete, Uvicorn listens on
8000, and `http://127.0.0.1:8000/api/health` reports persistent mode and status ok.
Redis should report ok when available. Swagger is at `/docs`.

```sh
python scripts/smoke_api.py
docker compose exec redis redis-cli SUBSCRIBE trades
docker compose logs api
docker compose down
```

The subscriber prints future executions while orders cross. `docker compose down`
retains the named PostgreSQL volume. Ordinary restarts replay history. Removing
that volume intentionally discards history; it is not part of the normal workflow.

Native persistent operation is also documented in the README. With local PostgreSQL
and Python already installed, the application does not require Docker to function.

## Verification status and limits

The local machine has no Docker CLI/daemon and no WSL installation. The Compose
file was parsed and its services/dependency/loopback configuration checked, but
**neither `docker compose build` nor the container stack ran locally**. A native
Uvicorn process with real PostgreSQL did pass an HTTP smoke test. Live Redis is
separately unverified here; simulated Redis tests passed.

`.github/workflows/ci.yml` builds the extension and C++ tests on Linux, runs Python
tests against PostgreSQL/Redis services, builds the Compose stack, and runs the
HTTP smoke script. It has been authored, not executed in a hosted CI environment.
The project is not currently a Git repository in this workspace.

Python package versions are pinned in `requirements.lock`. Base image tags and
Debian packages can receive updates, so builds are not bit-for-bit reproducible.
Digest locking and an image scan are reasonable future deployment checks. There
is no TLS termination, secret manager, backup automation, or rolling upgrade protocol.
Concurrent migration runners and multiple engine replicas are unsupported.

Interview exercise: Explain why a non-root runtime image is useful but does not
make an unauthenticated trading API production-ready. Then explain which process
owns the book during startup and why readiness must wait for replay.
