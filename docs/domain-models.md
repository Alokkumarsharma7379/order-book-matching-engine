# Domain models

The models are dependency-free C++20 value records in namespace `orderbook`.
They describe state; they do not submit orders or perform matching.

## Representation

| Type | Representation | Meaning |
| --- | --- | --- |
| `Price` | `std::int64_t` | Integer smallest price units; DEMO uses 100 units per 1.00 |
| `Quantity` | `std::int64_t` | Whole units of the instrument |
| `OrderId`, `TradeId` | `std::int64_t` | Separate positive ID namespaces |
| `SequenceNumber` | `std::int64_t` | Positive deterministic ordering value |
| `Timestamp` | `sys_time<microseconds>` | System-clock time at microsecond representation precision |

Signed 64-bit integers fit PostgreSQL BIGINT. The largest representable positive
value is 9,223,372,036,854,775,807; that is not a promise to accept every such
value in every operation. Arithmetic and counter exhaustion need explicit checks.
Signed overflow is undefined behavior; integer pricing does not eliminate it.
The core rejects negative prices/quantities rather than converting them to unsigned.

Price 10100 means 101.00 for DEMO. No floating-point conversion is needed for
crossing or equality comparisons. A limit order must have a positive price;
a market order must have `std::nullopt`. A trade always has an execution price.

The integer aliases document intent but are not distinct compiler-enforced types.
For example, assigning an OrderId to a TradeId compiles. Strong wrapper types
could prevent this, at the cost of more operators, hashing, and binding code.
The scoped enums prevent implicit conversion between Side, OrderType, and status.

## Order lifecycle and quantities

`original_quantity` records the initial request and stays unchanged by engine
convention. `remaining_quantity` is the unexecuted quantity after amendments.
`executed_quantity` is cumulative actual execution, independently of amendments.
`quantity_adjustment` is the net change to the initially requested quantity,
including accepted increases and reductions. It starts at zero and can be negative.

For example, after filling 10 of 50 units, the values are 50, 40, and 10.
If the user reduces the remaining quantity to 20, they become 50, 20, and 10.
Subtracting remaining from original would incorrectly claim 30 units executed.
In this example the amendment sets quantity_adjustment to -20. The invariant is
executed + remaining = original + quantity_adjustment. Validation compares
executed + remaining - original with the adjustment after checking the sum's bound.
Cancellation retains the remaining quantity and does not change this adjustment.
This field records a net amount, not the full history of individual amendments.

New and PartiallyFilled orders can rest. Filled and Cancelled orders must not.
Cancelled records retain unexecuted quantity, so positive remaining quantity
alone does not establish membership in the active book. A market order with
insufficient liquidity ends Cancelled, even if it executed some trades first.

`creation_sequence` stays unchanged. `priority_sequence` changes when a price
change or quantity increase loses FIFO priority. A same-price reduction keeps it.
Timestamps are provided explicitly; construction never reads the current clock.
Clock readings need not be monotonic, so sequences determine processing priority.
Microsecond representation does not guarantee microsecond clock accuracy.

## Trade records

Maker identifies the resting order; taker identifies the incoming order.
Execution price is the maker's price. One incoming order can generate many trades.
Trade IDs and execution sequences serve different purposes: one identifies the
trade, the other places its execution in the engine's event ordering.

## Ownership and validation boundary

These aggregates are copyable snapshots with no owning pointers or allocations.
All fields have deterministic defaults, but a default record is not an accepted
order or trade: zero IDs/quantities and a missing limit price are invalid inputs.
MatchingEngine validates commands before admitting or mutating state.
The future API will also validate input; public snapshots expose no live references.

Original quantities and emitted trades are logically immutable, rather than using
const data members. This keeps records assignable for containers and persistence.
Engine ownership and its public interface enforce those lifecycle rules.
Private fields and checked constructors are an alternative, but complete state
validation also requires book membership and command history unavailable here.

Constructing, copying, and accessing either fixed-size record takes O(1) time and
O(1) space. The build check constructs illustrative records; separate executable
checks cover matching and amendments. The Phase 7 Catch2 suite adds boundary,
reference-model, and allocation-failure coverage; see [testing.md](testing.md).

## Interview prompts

- Why does an optional price express market orders better than a zero sentinel?
- Why do quantity amendments require a separate executed quantity?
- Why can a Cancelled order have positive remaining and executed quantities?
- What is the difference between timestamp precision and FIFO priority?
- What safety do enum classes provide that integer aliases do not?
- Where will invalid inputs and arithmetic overflow be rejected?
