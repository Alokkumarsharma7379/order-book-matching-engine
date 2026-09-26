FROM python:3.14-slim-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY pyproject.toml CMakeLists.txt ./
COPY engine ./engine
COPY python ./python
COPY api ./api
COPY requirements.lock ./requirements.lock
RUN python -m pip wheel --wheel-dir /wheels -c requirements.lock .

FROM python:3.14-slim-bookworm AS runtime
RUN useradd --create-home --uid 10001 app
WORKDIR /app
COPY --from=build /wheels /wheels
RUN python -m pip install --no-index --find-links=/wheels orderbook-engine \
    && rm -rf /wheels
COPY alembic.ini ./
COPY migrations ./migrations
USER app
EXPOSE 8000
HEALTHCHECK --interval=10s --timeout=3s --start-period=20s --retries=3 \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/api/health', timeout=2)"
CMD ["python", "-m", "api.run"]
