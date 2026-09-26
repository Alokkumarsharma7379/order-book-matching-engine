# Phase 7: C++ unit tests

The suite registers 51 CTest entries: 46 Catch2 test cases, four earlier smoke
checks, and one standalone allocation-failure test. Catch2 generators run multiple
inputs inside a test case; these inputs are not counted as separate CTest entries.
The core implementation is unchanged in this phase.

## Files and integration

| File | Purpose |
| --- | --- |
| `engine/tests/CMakeLists.txt` | Pinned Catch2 dependency, unit executable, CTest discovery, allocation executable |
| `engine/tests/test_support.hpp` | Request factories, complete value comparisons, state snapshots |
| `engine/tests/matching_tests.cpp` | 17 matching, query, price/FIFO, and metadata cases |
| `engine/tests/order_management_tests.cpp` | 10 cancellation, amendment, and priority cases |
| `engine/tests/validation_tests.cpp` | 12 rejection, duplicate-ID, and integer-boundary cases |
| `engine/tests/order_book_tests.cpp` | Six storage, snapshot, validation, and growth cases |
| `engine/tests/reference_model.hpp` | Independent, deliberately simple matching oracle |
| `engine/tests/reference_tests.cpp` | One generated-workload case covering four fixed seeds |
| `engine/tests/allocation_failure_check.cpp` | Exception-safety checks with isolated allocation injection |

The root `CMakeLists.txt` adds test-only dependencies, shared warning flags, and
optional sanitizer flags. `CMakePresets.json` adds a separate sanitizer build.
This guide and the existing build/domain/storage/matching/amendment guides document
the verification and its scope.

Catch2 v3.8.1 is fetched from its official source archive and checked against
SHA-256 `18b3f70ac80fccc340d8c6ff0f339b2ae64944782f8d2fca2bd705cf47cadb79`.
The integration uses `Catch2::Catch2WithMain` and `catch_discover_tests` as described
in the [official CMake integration documentation](https://catch2-temp.readthedocs.io/en/latest/cmake-integration.html).
Discovery runs before CTest execution, after the binary is built. This supplies a
failure report per named test, while keeping the assertion runner out of the core.
`BUILD_TESTING=OFF` excludes all test executables and the Catch2 download.

## Required behavior coverage

| Requirement | Test coverage |
| --- | --- |
| Resting limit buy | `Limit buy rests without an ask` |
| Resting limit sell | `Limit sell rests without a bid` |
| Crossing buy | `Crossing buy executes at the resting sell price` |
| Crossing sell | `Crossing sell executes at the resting buy price` |
| Best price first | `Best price outranks arrival time on either side` |
| FIFO at a price | `FIFO survives equal and backward timestamps and partial fills` |
| Partial fill | Maker cached totals and incoming resting remainder cases |
| Multiple orders at one level | `One taker fills several orders at an identical price` |
| Multiple price levels | Required 20/30/50 sweep, in both directions |
| Market buy | `Market buy consumes available asks` |
| Market sell | `Market sell consumes available bids` |
| Insufficient market liquidity | Unexecuted remainder cancelled, including an empty book |
| Cancellation | Head/middle/tail, sole best order, and partially filled order cases |
| Unknown cancellation | `Unknown and inactive orders reject cancellation and amendment` |
| Price amendment | Repricing to the back of the destination queue and crossing amendments |
| Quantity amendment | Same-price increases/reductions and cumulative accounting |
| Amendment priority | Reduction retains priority; increase/reprice loses it; no-op consumes no sequence |
| Empty book | Empty queries, empty best prices, and market expiry |
| Large quantities | Exact signed 64-bit maximum matching and overflow rejection |
| Zero/negative quantities | Zero, minus one, and minimum signed value for limit/market requests |
| Duplicate IDs | Active, filled, and cancelled order IDs are rejected |
| Integrity after many operations | 4,096 stored orders and 2,400 mixed commands checked against the oracle |

Full fills are covered by crossing buy/sell and maximum-quantity cases.
Additional cases check integer overflow in level totals and amendments, generated
ID exhaustion, invalid prices/enums, copied query results, pagination, and all
order/trade metadata. Rejection tests compare before/after state where applicable.
The tests check observable matching outcomes as well as structural invariants.

## Why a separate reference model?

An invariant check can prove that the lists and indexes agree while still missing
a matching policy bug. The reference model stores orders in a plain vector and
scans all active orders for the best price and oldest priority on each fill. It
recomputes level totals instead of maintaining caches. It shares domain types but
does not call `OrderBook` or the engine's matching helpers.

Four fixed seeds (`7`, `42`, `2026`, `0xC0FFEE`) each generate 600 commands. Workloads
mix limit/market submissions, cancellation, repricing, quantity changes, and no-ops,
with equal and backward timestamps. Requests and active-ID selection come from
the model. After every command, tests compare returned orders/trades, every retained
order field, complete trade history, active count, and both sides' aggregated levels.
The engine also checks its invariants. A failure includes the seed and step.

The model intentionally accepts only bounded, valid commands; explicit fixtures
cover invalid inputs and numeric extremes. Fixed seeds make regressions reproducible
but are not exhaustive fuzzing or proof of correctness.

## Allocation failure checks

`allocation_failure_check` is a separate executable because it replaces global
ordinary `new`/`new[]`. Catch2's own allocations must not affect the failure counter.
Each attempt constructs a fresh fixture, arms a one-shot failure for the command's
Nth allocation, then disarms before inspecting or reporting results.

The check tries every reached allocation position until a command succeeds. On
`std::bad_alloc`, it requires unchanged logical state, a healthy engine, and valid
invariants. It retries the same command and follows it with submissions and a fill,
comparing all fields against a control engine to detect consumed order IDs, trade
IDs, or event sequences. Container capacity changes are allowed.

The 24 scenario/side combinations cover submission at existing/new levels,
market sweeps, resting remainders, empty markets, repricing, crossing amendments,
cancellation, reduction, increase, and no-op amendment. Successful cancellation and
the selected same-price amendment fixtures also run with the first allocation
forced to fail, verifying that those command paths do not allocate.

The injector covers ordinary C++ allocation calls reached by these fixtures, not
over-aligned allocation, direct `malloc`, OS failures, or every possible book shape.
Its failure-point count may vary with compiler and standard-library implementation.

## Build and run

From the repository root in x64 PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
& .\scripts\setup-windows.ps1
cmake --preset debug
cmake --build --preset debug --parallel 2
ctest --preset debug
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release
cmake --preset sanitizers
cmake --build --preset sanitizers --parallel 2
ctest --preset sanitizers
```

Expected summary for each configuration:

```text
100% tests passed, 0 tests failed out of 51
```

The first configure of each build directory downloads Catch2. Subsequent builds
reuse it. Setup selects the compiler and places its runtime DLL directory on PATH;
run it in each new shell. Sanitizers use AddressSanitizer and UndefinedBehaviorSanitizer
with recovery disabled. The core and project test targets are instrumented; Catch2
itself is built with its usual flags. This preset requires a supported GCC/Clang
toolchain with GNU-style flags and available sanitizer runtimes. It does not support
MSVC/clang-cl. Sanitizer support elsewhere depends on the selected toolchain.

Useful focused commands after building:

```powershell
ctest --preset debug -L unit
ctest --preset debug -L allocation -V
& .\build\debug\engine\tests\engine_tests.exe '[reference]'
& .\build\debug\engine\tests\engine_tests.exe '[modify]'
& .\build\debug\engine\tests\engine_tests.exe '[boundary]'
```

To build only the library, with the compiler already selected:

```powershell
cmake -S . -B build/core-only -G Ninja -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/core-only --parallel 2
```

## Verification performed

Verified on x64 Windows with LLVM-MinGW Clang 21.1.1, CMake 3.31.6, and Ninja 1.13.0:

| Configuration | Result |
| --- | --- |
| Debug | 51/51 CTest entries passed |
| Release | 51/51 CTest entries passed |
| Debug with AddressSanitizer and UndefinedBehaviorSanitizer | 51/51 passed; no sanitizer diagnostics |
| Release library with `BUILD_TESTING=OFF` | Built successfully without fetching Catch2 |

In each full suite, all four reference-model seeds passed (2,400 mixed commands
total). The standalone allocation check reported:

```text
Allocation failure checks passed (56 injected failure points, 24 scenario/side combinations)
```

These results describe the tested workloads and this toolchain, not a guarantee
that every failure mode or other platform is covered.

No benchmark results are inferred from test duration. Sequence/trade-counter
exhaustion and the defensive unhealthy state after an internal commit bug are not
forced through private test hooks. Concurrency, Python bindings, HTTP, persistence,
and Redis are outside this phase.

## Questions to check understanding

- Why can correct cached totals coexist with an incorrect FIFO policy?
- Why must a failed submission preserve counter values as well as order quantities?
- Why test a maximum integer directly instead of trying to reach it with a large loop?
- Why keep the fault injector separate from the assertion framework?
- Which bugs could remain even when all four reference-model seeds pass?
