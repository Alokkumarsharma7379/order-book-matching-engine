# Phase 6: cancellation and modification

The public entry points are MatchingEngine::cancel(id, timestamp) and
MatchingEngine::modify(id, ModifyOrder). Both operate on active limit orders.
The engine remains single-threaded and returns copies, not mutable references.

## Contract

Cancel returns an Order with status Cancelled. It removes the active location and
FIFO node, updates the cached level total, and deletes an empty price level.
The historical record, remaining quantity, executed quantity, original quantity,
ID, creation metadata, and priority sequence are preserved. updated_at changes.
No trade is generated. The cancelled ID cannot be reused for another submission.

ModifyOrder contains optional price, optional remaining_quantity, and timestamp.
Omitted fields retain their current values. At least one changed-field argument
must be supplied, although supplying identical values is a valid no-op.
Zero/negative price or remaining quantity is rejected; use cancel to remove an order.
Changing side/type is not part of the modification interface.

| Request | Priority and execution |
| --- | --- |
| Same price, smaller remaining quantity | Keep the node and priority sequence |
| Same price, larger remaining quantity | Move to the queue's back with a new priority sequence |
| Changed price, any valid remaining quantity | New priority; match immediately; rest any remainder at the back |
| Identical values | Return unchanged state; do not alter timestamps or consume a sequence |

The ID, original_quantity, creation_sequence, and created_at remain unchanged.
A successful cancellation or actual amendment consumes an event sequence.
Only priority-losing amendments replace priority_sequence. Resulting trades get
later sequences. All timestamps come from the caller; wall-clock order is irrelevant.
Modification returns ExecutionResult, including any trades produced by repricing.

## Accounting

Order now includes signed quantity_adjustment, the net amendment to initial demand.
The invariant is executed + remaining = original + quantity_adjustment.
The implementation checks executed + remaining for overflow before calculating
its difference from original, avoiding unsafe arithmetic with arbitrary deltas.

Example: original 50, executed 10, remaining 40, adjustment 0.
Reducing remaining to 20 gives original 50, executed 10, remaining 20, adjustment -20.
Increasing remaining to 60 then gives original 50, executed 10, remaining 60,
adjustment +20. Later fills transfer remaining to executed; adjustment stays +20.
Cancellation also leaves that adjustment unchanged and retains the unfilled amount.
This net adjustment does not replace a future persistent command journal.

## Implementation and failure behavior

Cancellation finds the location by ID, finds its price level, and erases the known
list node. It never scans the book. The order record remains available for lookup.

Amendment first validates numeric bounds. Priority-losing amendments also plan
fills against the opposite side. It reserves trade-history capacity and creates any destination level before moving
the original node. Destination totals exclude the old quantity when source and
destination are the same level. Repricing reuses the node with std::list::splice;
same-price increases splice to the same queue's end. List iterators remain valid.
Same-price reductions perform no splice. Empty source levels are removed.

The record and location already exist, so amendment needs no new hash-table entry.
Order copying/assignment is checked at compile time to be nonthrowing. After
preparation, relocation, scalar updates, and maker fills use no new allocations.
If the amended order fully fills, its old active node is removed instead of moved.

Rejected requests and allocation failures during preparation leave logical state
and counters unchanged. Unexpected commit failures stop the engine as in Phase 5.
Phase 7 adds allocation-failure injection and sanitizer checks; see
[testing.md](testing.md) for their scope. Tests exercise numeric rejection,
priority, repeated relocation, and structural invariants.

## Errors

- Nonpositive ID or invalid/empty amendment: std::invalid_argument.
- Unknown positive ID: UnknownOrderId, derived from std::out_of_range.
- Filled/cancelled order, including an expired market order: OrderNotActive.
- Unrepresentable requested total, level total, or event counter: std::overflow_error.

Repeated cancellation is rejected rather than treated as a successful no-op.
The API maps unknown orders to 404 and inactive-order conflicts to 409.
HTTP and persistence are implemented outside this core; see [service.md](service.md).

## Complexity and trade-offs

For L levels and F new fills, cancellation and same-price reductions take expected
O(log L) time and O(1) additional space. Repricing/increases take expected amortized
O(log L + F) time with O(F) fill-plan space and at most one new price level.
Hash lookup is expected O(1), and history growth can occasionally copy earlier trades.

Reusing a node avoids an allocation and preserves its iterator. Rebuilding a new
node would be simpler in some designs but would require careful rollback and index
replacement. Copying the entire book before amendment would make rollback easier
but impose O(book size) work per command; this design avoids that cost.

## Verification and interview prompts

order_management_check covers both sides, head/middle/tail cancellation, partially
filled orders, FIFO retention/loss, target-level FIFO, crossing amendments, resting
remainders, no-ops, terminal/unknown orders, overflow rejection, and 256 orders
under repeated amendments and cancellations. Existing checks run alongside it.
The Phase 7 Catch2 suite extends these checks; no benchmark results are claimed.

- Why does increasing quantity lose time priority?
- Why does a same-price reduction still take O(log L) with cached level totals?
- What happens to the stored iterator after splice across two price levels?
- Why must target-level capacity be checked before unlinking the old order?
- How can cumulative execution exceed original quantity without violating accounting?
