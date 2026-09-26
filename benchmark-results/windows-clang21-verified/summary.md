# Measured local benchmark

Median across trials; latency columns are medians of per-trial statistics.

| Scenario | Commands/s | Submitted orders/s | Mean ns | Median ns | p95 ns | p99 ns |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| mostly_noncrossing | 3,037,641.2 | 3,037,641.2 | 298.7 | 100.0 | 300.0 | 600.0 |
| frequent_crossing | 2,491,203.6 | 2,491,203.6 | 372.0 | 300.0 | 400.0 | 600.0 |
| same_price | 3,463,913.6 | 3,463,913.6 | 260.3 | 100.0 | 200.0 | 200.0 |
| many_levels | 2,613,042.8 | 2,613,042.8 | 355.6 | 200.0 | 200.0 | 400.0 |
| heavy_cancellation | 7,851,709.5 | 1,962,927.4 | 100.4 | 100.0 | 200.0 | 300.0 |

Includes per-command clock and harness overhead. Core only; excludes Python, HTTP, SQL, and Redis.
Single developer-machine run; not a production capacity claim. See raw.csv and metadata.json.
