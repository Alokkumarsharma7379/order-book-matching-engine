# OrderBook storage

The public OrderBook insertion stores valid, non-crossing limit orders.
MatchingEngine owns a book and supplies matching, IDs/sequences, and terminal history.
Use MatchingEngine::submit for execution; public add_resting still rejects crossing.
MatchingEngine also exposes cancellation and amendments, added in Phase 6.

## Containers and ownership

- Bids: descending `std::map<Price, PriceLevel>` via `std::greater<Price>`.
- Asks: ascending `std::map<Price, PriceLevel>`.
- Each level: `std::list<OrderId>` in FIFO order, plus cached remaining quantity.
- Records: `std::unordered_map<OrderId, Order>` owns the order values.
- Locations: `std::unordered_map<OrderId, Location>` stores side, price, list iterator.

The first map entry is always the best price. Append to a level's list to join
the back of its queue. The caller must provide increasing priority sequences at
each level; timestamps never determine FIFO. Global sequence allocation belongs
to MatchingEngine. No filled/cancelled/market order is accepted as resting.

Queries return copies. Changing a snapshot cannot mutate the live book, and no
public API exposes a container iterator. Copy and move of OrderBook are disabled
to avoid accidental duplication or transfer of its iterator-bearing indexes.
The class is single-threaded; it provides no synchronization.

## Cached totals and numeric limits

This refines Phase 1's proposed on-demand aggregation: each level caches its total.
It avoids scanning a queue to reject an overflowing aggregate during insertion
and makes aggregated snapshots O(L). It adds an invariant: cached total must
equal the sum of all remaining quantities at that level. Fills, removals, and
amendments maintain it too. The diagnostic independently recomputes it.

Each level total must fit signed 64-bit Quantity. A single maximum-size order is
supported; adding one more unit at that price throws std::overflow_error before
state changes. Another level can independently hold its own total. Each order's
executed plus remaining quantity must also fit Quantity. No unchecked addition
is used for the cached total. Original quantity need not equal that sum after
an amendment; quantity_adjustment records that net difference.

## Insertion and iterator safety

Insertion validates the record, duplicate ID, and crossing condition, locates the
price level, and checks its total and FIFO sequence. It then stores the record,
appends its ID, and stores the location. If an allocation throws, rollback removes
any inserted record/list node and any newly created empty level, then rethrows.
The logical state is preserved; private hash-table bucket capacity may change.

List insertion preserves existing node iterators. Hash-table rehashing does not
affect iterators into the separate lists. No hash-table iterator is kept across
an insertion into that same hash table. Removal erases the location
when its list node is erased and remove the level when its final node is gone.
The location index enables cancellation without scanning a queue.

`check_invariants()` checks record/location membership, uniqueness, side/price,
iterator correspondence, increasing priority, totals, nonempty levels, and an
uncrossed book. Terminal records remain in the record table, but must be absent
from the active index and queues. Every inactive record must be Filled or Cancelled.
This diagnostic is not a defense against arbitrary memory corruption and is not
automatically run on every insertion.

## Complexity

Let L be price levels, N active orders, H retained records, and Q orders at the queried price.
Hash lookups have expected O(1) cost, not a worst-case guarantee.

| Operation | Expected time | Output/extra space |
| --- | --- | --- |
| Insert resting order | O(log L), amortized over hash growth | O(1) persistent increase |
| Query order | O(1) | O(1) copy |
| Best bid/ask, active count | O(1) | O(1) |
| Aggregated side snapshot | O(L) | O(L) |
| FIFO snapshot at one price | O(log L + Q) | O(Q) |
| Explicit invariant check | O(H + N + L) | O(N) |

Total storage is O(H + N + L). Cancellation uses expected O(1) ID lookup,
O(log L) price lookup, and O(1) list-node removal; it needs no full-book scan.
Maps/lists trade locality and allocation overhead for simple ordering and stable
nodes. A vector improves locality but complicates middle removal; a heap needs
additional indexing or lazy deletion for arbitrary cancellation.

## Verification and interview prompts

`order_book_check` covers empty queries, best prices, FIFO, snapshot independence,
invalid/duplicate/crossing rejection, quantity overflow, and index integrity
after 2,048 additional insertions. Checks run in Release as well as Debug and
do not depend on assert. Allocation-failure injection is not covered here.
The Phase 7 Catch2 suite extends this coverage. This executable checks storage only;
the matching_check executable added in Phase 5 separately verifies execution.

## Matching integration added in Phase 5

MatchingEngine is the sole friend of OrderBook. It can stage a resting remainder
before applying a validated fill plan, temporarily bypassing the public crossing
check. This is private to the single-threaded engine and not exposed to callers.
After commit, the public uncrossed-book invariant holds again. It also removes
filled makers' active locations, list nodes, and empty levels while retaining
their records. It uses IDs to reacquire records after insertion may rehash storage.

Phase 6 adds private active-node removal and replacement. Replacement validates
the destination total and allocates any new map level before moving the existing
list node with splice. All lists use the same standard allocator. The node's
iterator remains valid after a move between levels; its location's price changes.
Same-price reductions leave the node in place. A fully executed replacement
removes the active node and retains the historical order record.

- Why are locations stored separately from price levels?
- Which iterators remain valid when the record hash table rehashes?
- Why is cancellation O(log L) in this design rather than simply O(1)?
- What additional invariant does caching quantities introduce?
- What must be undone if storing the location throws after list insertion?
