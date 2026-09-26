# Phase 17: explain the project yourself

Use these as spoken exercises. Give a two-minute answer, point to the relevant
code or test, then identify one trade-off. Do not memorize a scripted answer.

## Round 1: matching and data structures

1. Why did you choose `std::map` for price levels? Describe the ordering on each
   side and compare lookup/insertion with a hash map.
2. Why not `priority_queue`? Trace what happens when cancelling an arbitrary order
   that is not at the best price or the front of its level.
3. Is cancellation O(1) or O(log L) in this implementation? Account for both ID
   lookup and price-level lookup instead of quoting only the list erase cost.
4. How does FIFO survive equal or backward timestamps? Give a concrete sequence
   of three orders and a partial fill.
5. During a partial fill, which order fields, cached totals, queue entries, and
   location indexes change? Which remain unchanged?
6. Why should prices not use `double`? Explain the meaning and limits of one price unit.
7. Who is the maker and who is the taker? Why can a crossing buy execute below its limit?
8. What happens to unfilled market quantity? How can you distinguish a fully filled
   market order from one that expired with a remainder?

Trace this without running it first: A buys 5 at 100; B buys 4 at 100; A increases
its remaining quantity to 7; C market-sells 6. Write the trade order, quantities,
remaining book, and A's immutable original quantity. Then write a test to check it.

## Round 2: backend boundaries and recovery

9. What would you change for a target of one million orders/s? First distinguish
   standalone core throughput, durable command acknowledgement, and HTTP capacity.
   Identify the next measurement instead of promising a redesign's speed.
10. Why keep PostgreSQL outside the matching loop? Which database work still blocks
    the API command and why?
11. What happens if Redis is down? Describe both the command response and what a
    disconnected subscriber can recover later.
12. Why use a single-threaded core? Explain how the GIL, service lock, and database
    advisory lock have different responsibilities.
13. How would you scale across symbols? Where would a producer/consumer queue sit,
    and which ordering guarantee would remain per symbol?
14. How does this version recover after a crash? Compare a crash before SQL commit,
    after commit but before response, and after commit but before Redis publication.
15. How would you guarantee ordering across distributed services? Identify a
    sequencer/partition owner, retries, durable logs, and failover as separate concerns.

## Evidence and self-check

| Topic | Inspect after answering | A strong answer includes |
| --- | --- | --- |
| Storage and iterators | `engine/src/order_book.cpp` | Stable list nodes, hash rehash rules, removal of stale locations |
| Matching and amendment | `engine/src/matching_engine.cpp` | Plan/prepare/commit, maker price, retained/lost priority |
| Boundary validation | `api/schemas.py`, `api/tests/test_api.py` | Strict integers, semantics, centralized status codes |
| Persistence | `api/service.py`, `api/tests/test_persistence.py` | Uncertain outcomes, fail-stop, deterministic replay, retry keys |
| Redis | `api/events.py` | Pub/Sub loss, post-commit gap, reconciliation |
| Benchmark | `engine/benchmarks/benchmark.cpp` | Workload shape, measurement overhead, percentiles, actual limits |

Score each answer 0–3: 0 cannot explain; 1 states a rule; 2 traces this implementation;
3 traces it and defends a trade-off with a test or measurement. Revisit any answer
below 2 by tracing a concrete order sequence through the code.

Practice a five-minute project walkthrough: problem, core representation, one
matching example, API/storage boundary, an actual failure test, one measured result,
and two limitations. Do not describe this project as production trading infrastructure.
