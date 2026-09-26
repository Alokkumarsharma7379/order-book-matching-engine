# Phase 14: measured benchmark

The standalone `orderbook_benchmark` executable uses `std::chrono::steady_clock`.
It measures the C++ engine only. No Python conversion, HTTP, database transaction,
Redis call, or durable acknowledgement is included.

## Reproduce

Select the compiler first (`scripts/setup-windows.ps1` on Windows), then:

```powershell
cmake --preset release -DORDERBOOK_BUILD_BENCHMARKS=ON
cmake --build --preset release --parallel 2
& .\.venv\Scripts\python.exe scripts/benchmark.py `
  --executable build/release/orderbook_benchmark.exe `
  --commands 250000 --trials 7 --warmup 10000
```

On Linux use `build/release/orderbook_benchmark` without `.exe`. The C++ binary also
accepts `[commands] [trials] [warmup]` directly and writes CSV to stdout, compiler
information to stderr. The Python runner creates a new timestamped artifact folder
under `benchmark-results/`; an explicit `--output` directory must not already exist.

## Workloads and timing

| Workload | Measured operations |
| --- | --- |
| mostly_noncrossing | Limit submissions on both sides; every twentieth submission crosses |
| frequent_crossing | Alternating sell/buy at the same price, pairing fills |
| same_price | All buys at one level; FIFO/storage grows |
| many_levels | One new buy level per submission |
| heavy_cancellation | 75% cancels, 25% submissions; begins with an untimed seeded book |

Each scenario runs an untimed warmup on a separate engine. Each measured trial
uses a fresh engine. Command generation, cancellation prefill, latency vector
allocation, sorting, output, invariant checks, and destruction of the final engine
are outside timing. Command results and their destruction, allocations performed
by matching, retained-history growth, and clock calls are included.

Per-command latency brackets one submit/cancel operation. The overall duration
also includes recording each latency and loop overhead. `commands_per_second`
counts all commands; `orders_per_second` counts only submitted orders, so these
differ for cancellation-heavy workloads. No trade-per-second or API-throughput
claim is inferred. Mean latency uses all samples; median/p95/p99 use nearest-rank
quantiles. The summary reports medians across trial metrics, not pooled quantiles.

## Actual local results

The verified run used x64 Windows, Clang 21.1.1 Release, 250,000 commands per trial,
seven trials per scenario, and 10,000 warmup commands. Raw evidence:

- [CSV for all 35 trials](../benchmark-results/windows-clang21-verified/raw.csv)
- [Compiler, platform, source hashes, and executable hash](../benchmark-results/windows-clang21-verified/metadata.json)
- [Complete measured summary](../benchmark-results/windows-clang21-verified/summary.md)

| Scenario | Median commands/s | Mean ns | Median ns | p95 ns | p99 ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| mostly_noncrossing | 3,037,641.2 | 298.7 | 100 | 300 | 600 |
| frequent_crossing | 2,491,203.6 | 372.0 | 300 | 400 | 600 |
| same_price | 3,463,913.6 | 260.3 | 100 | 200 | 200 |
| many_levels | 2,613,042.8 | 355.6 | 200 | 200 | 400 |
| heavy_cancellation | 7,851,709.5 | 100.4 | 100 | 200 | 300 |

Cancellation-heavy submitted-order throughput was 1,962,927.4/s; its higher
command throughput must not be presented as that many submitted orders per second.

The observed timer readings are quantized in 100 ns steps. Small per-command
percentiles therefore have limited resolution. Occasional allocation/rehash or OS
outliers can lift the mean above p99 when they affect fewer than 1% of samples.
The workload, final book size, allocator, CPU power state, scheduling, and warm
caches all matter. CPU affinity and frequency were not controlled. These short
single-machine trials are not production exchange capacity or end-to-end latency.

An initial development run in `windows-clang21/` formatted long-double mean
latency as zero with this Windows toolchain. It is explicitly marked invalid.
The benchmark now accumulates/formats double values, retains duration precision,
and its runner rejects nonpositive means and invalid percentile ordering. Only
the verified rerun is used above.

Interview exercise: Explain why dividing one by median latency does not necessarily
equal measured throughput, and why a p99 measured here says nothing about p99 HTTP
latency while PostgreSQL is committing transactions.
