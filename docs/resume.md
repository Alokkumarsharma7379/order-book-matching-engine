# Evidence-based resume wording

Use only claims you can explain and reproduce. Suitable behavior-focused bullets:

- Built a C++20 order-book engine implementing price-time priority, limit/market
  orders, partial fills, cancellation, and priority-aware amendments; verified it
  with 46 Catch2 cases, reference-model workloads, and allocation-failure tests.
- Exposed the engine through pybind11 and FastAPI, with PostgreSQL command replay,
  idempotent retries, and Redis trade-publication code; tested rollback, restart,
  writer ownership, and simulated Redis failures.

Optional measured bullet, only with its scope retained:

- Measured 2.49 million submitted orders/s in a local C++ Release benchmark with
  alternating crossing orders, using seven trials of 250,000 commands and recorded
  latency distributions; excluded HTTP, persistence, and Redis overhead.

The performance number comes from
[the retained local results](../benchmark-results/windows-clang21-verified/summary.md).
Do not change it into an API, production, HFT, or guaranteed capacity claim.
Docker configuration and CI are present, but their end-to-end execution is unverified
in this environment. Do not claim a completed deployment or live Redis integration
test until those checks actually run. Interview readiness also requires explaining
the code yourself; see [the exercises](interview.md).
