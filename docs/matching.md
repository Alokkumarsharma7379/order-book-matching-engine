# Matching

MatchingEngine owns one DEMO book and processes commands synchronously.
There are no database, Python, networking, or Redis dependencies in the core.
Phase 6 adds cancellation/modification; their contract is in order-management.md.
Submission and repricing reuse the same fill planner and commit implementation.

## Command and result

SubmitOrder contains side, type, optional price, positive quantity, an explicit
timestamp, and an optional positive order ID. Market orders must omit price.
Missing IDs are generated above the largest accepted ID; caller-supplied IDs
remain useful for deterministic replay and duplicate-ID tests. IDs cannot be
reused while history is retained, including after orders become terminal.

The timestamp is supplied by the caller; it is not automatically the current time.
Each accepted submission gets a creation/priority sequence, then each resulting
trade gets a later sequence. Sequences, not timestamps, determine ordering.
All trades from a submission share its supplied timestamp. Makers' updated_at
values also use it, even if the wall clock moves backward.

ExecutionResult returns the incoming order's final state and its executed trades.
find_order returns current state for any accepted ID. bids/asks return aggregate
snapshots. trades(after_id, limit) returns a copied, ascending page of trades with
IDs strictly greater than after_id; the default limit is 100. Zero limit is empty.
The trade IDs identify maker records that persistence will need to update later.

## Matching algorithm

1. Validate the command and allocate counter values locally.
2. Visit the best opposite price, then its FIFO queue.
3. Stop at an unacceptable price for a limit order, or when quantity is exhausted.
4. Plan each fill as min(incoming remaining, maker remaining), at the maker price.
5. Reserve history capacity and stage the final incoming record/resting remainder.
6. Apply the plan to makers; reduce level totals and remove filled active nodes.
7. Append trade history and publish the new counter values inside the engine.

A limit buy accepts ask prices <= its limit. A limit sell accepts bids >= its limit.
Market orders have no price boundary. They never rest: an unexecuted remainder
ends Cancelled, retaining its remaining quantity and any successful executions.
A completely executed order is Filled. A partially executed limit remainder is
PartiallyFilled and joins the back of its own price queue with its arrival sequence.
A partial maker retains its existing queue node and time priority.

For asks 20 @ 9900, 30 @ 10000, and 70 @ 10100, a buy of 100 with limit 10100
executes 20, 30, and 50 at those respective prices. The final ask is 20 @ 10100.
This scenario is executed by matching_check; it is not a benchmark.

## Failure and ownership rules

The plan contains IDs and value records, not hash-table iterators or pointers.
Staging can rehash order storage without invalidating the plan. FIFO removal uses
the current front of the opposite level; empty levels are erased by iterator.

Validation, duplicate IDs, numeric limits, and allocation failure during planning
or staging leave logical state and counters unchanged. Private container capacity
may grow. History grows geometrically rather than reserving exactly one more
element on every submission. Commit uses existing capacity and nonthrowing Trade
copies; result movement is also checked at compile time to be nonthrowing.

Staging may temporarily create a crossed internal book. No external observer can
access it because the engine is synchronous, owns the book, and exposes only copied
queries. Calling it concurrently is unsupported; the future API must serialize access.

An unexpected internal commit failure sets healthy() to false. Submissions, queries,
and invariant checks then refuse access; recreate/recover the engine rather than
continue with uncertain state. This is a defensive stop, not transactional rollback
after an internal programming error. The core has no persistence; the Python service
now provides durable command replay as described in [service.md](service.md).
Phase 7 adds allocation-failure injection and sanitizer checks; see
[testing.md](testing.md) for the fixtures and verification scope.

## Invariants and bounds

OrderBook checks active membership, locations, FIFO, totals, terminal exclusion,
and the absence of crossed prices. MatchingEngine additionally checks trade IDs,
execution sequences, opposite sides, and per-order cumulative executed quantities.
After amendments, executed + remaining must equal original + quantity_adjustment.
The original request stays unchanged; executed quantity still agrees with trades.

Prices, quantities, IDs, and sequences use signed 64-bit integers. Counters are
checked before increment, and retained level totals must fit Quantity. Filling
transfers quantity from remaining to executed without exceeding their validated
sum. Prices are compared, never multiplied by quantity in the matching path.

For F fills and L price levels, expected amortized matching work is O(F + log L),
including planning and staging; erasing a map node by iterator is amortized constant.
Individual submissions may also pay for hash rehashes or copying growing trade
history. These are not worst-case latency guarantees. Trade pagination costs
O(log T + R) for T retained trades and R returned records.

Memory is O(H + A + L + T) for historical orders H, active orders A, levels L,
and trades T, plus O(F) temporary fill-plan storage. The full invariant diagnostic
takes expected O(H + A + L + T) time and O(H + A) additional space.
History is currently unbounded; archiving is a future concern.

## Interview prompts

- Why execute at the resting price rather than the incoming limit?
- How does a partial maker keep priority?
- Why plan fills before changing makers? What extra memory does that cost?
- Which allocations can occur before the commit pass?
- Why retain filled records after removing their active nodes?
- Why does single-threaded ownership simplify staging and deterministic replay?
