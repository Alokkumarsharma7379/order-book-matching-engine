# Measured local benchmark

Median across trials; latency columns are medians of per-trial statistics.

| Scenario | Commands/s | Submitted orders/s | Mean ns | Median ns | p95 ns | p99 ns |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| mostly_noncrossing | 4,135,546.7 | 4,135,546.7 | 0.0 | 100.0 | 200.0 | 400.0 |
| frequent_crossing | 4,188,762.4 | 4,188,762.4 | 0.0 | 100.0 | 200.0 | 300.0 |
| same_price | 3,976,396.1 | 3,976,396.1 | 0.0 | 100.0 | 200.0 | 200.0 |
| many_levels | 2,523,812.2 | 2,523,812.2 | 0.0 | 200.0 | 300.0 | 500.0 |
| heavy_cancellation | 6,442,302.7 | 1,610,575.7 | 0.0 | 100.0 | 200.0 | 300.0 |

Includes per-command clock and harness overhead. Core only; excludes Python, HTTP, SQL, and Redis.
Single developer-machine run; not a production capacity claim. See raw.csv and metadata.json.
