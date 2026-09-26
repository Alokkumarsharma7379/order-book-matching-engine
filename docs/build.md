# Building the project

This is an exchange-style educational matching engine in development.
The core includes the Catch2 suite, reference-model checks, and allocation-failure tests.
The completed Python/API setup is described in [the README](../README.md).
The standalone core supports limit/market matching, history, and invariant checks.

## Windows setup

Run from the repository root in x64 PowerShell with Python on PATH:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
& .\scripts\setup-windows.ps1
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
& .\build\debug\build_check.exe
& .\build\debug\order_book_check.exe
& .\build\debug\matching_check.exe
& .\build\debug\order_management_check.exe
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --preset sanitizers
cmake --build --preset sanitizers --parallel 2
ctest --preset sanitizers
```

The execution policy change lasts only for this terminal session.
Run setup again in each new terminal to select the workspace's tools.
The first setup requires internet access; subsequent runs reuse the compiler.
The first test-enabled CMake configure in each build directory also downloads
the pinned Catch2 v3.8.1 archive and verifies its SHA-256 digest.
Tools live in ignored `.tools/`; the installed system compiler is unchanged.
The compiler archive is verified against its release SHA-256 digest.
The script targets x64 Windows; it is not an ARM64 setup script.

Expected build_check output:

```text
C++20 build check passed
Domain models compiled (sample IDs: order 1, trade 1)
```

Expected order_book_check output: `OrderBook storage checks passed`.
Expected matching_check output: `Matching checks passed`.
Expected order_management_check output: `Cancellation and modification checks passed`.
CTest should report 51 passing entries in each configuration: 46 Catch2 cases,
four smoke checks, and the allocation-failure executable. Catch2 generators expand
many cases across inputs without increasing the registered CTest count.
The build_check uses hand-constructed records; matching_check executes actual orders.
These checks do not measure performance. See [testing.md](testing.md) for the
coverage map, focused commands, reference model, and failure-injection scope.

## Other toolchains

With a C++20 compiler, CMake 3.21 or newer, and Ninja on PATH, use the same
configure, build, and test presets. Select the compiler before configuring,
using `CXX` or an appropriate compiler developer shell.
The presets do not hard-code Windows paths.

Debug, Release, and sanitizers use separate directories under `build/`.
The sanitizer preset requires GCC/Clang with GNU-style flags and available
AddressSanitizer/UndefinedBehaviorSanitizer runtimes; it does not support MSVC/clang-cl.
`BUILD_TESTING=OFF` builds the library without fetching Catch2 or building tests.
CMake caches the selected compiler: use a new build directory if changing it.
The portable LLVM-MinGW setup has also built the extension for CPython 3.14.3;
see [python-bindings.md](python-bindings.md) for the verified ABI and runtime details.

## Layout

- `engine/tools/build_check.cpp`: small C++20 verification executable.
- `engine/tools/order_book_check.cpp`: executable storage verification.
- `engine/tools/matching_check.cpp`: executable matching verification.
- `engine/tools/order_management_check.cpp`: cancellation and amendment verification.
- `engine/include/orderbook/`: public domain and OrderBook headers.
- `engine/src/order_book.cpp`: OrderBook storage implementation.
- `engine/src/matching_engine.cpp`: matching and trade-history implementation.
- `CMakeLists.txt`: language level, warning flags, target, and CTest registration.
- `engine/tests/`: Catch2 cases, independent reference model, and allocation-failure executable.
- `CMakePresets.json`: repeatable Debug, Release, and sanitizer commands.
- `requirements-build.txt`: pinned CMake and Ninja versions.
- `scripts/setup-windows.ps1`: isolated Windows tool installation and selection.

Python bindings, API, database, Docker configuration, and benchmarks are now present.
See [the implementation record](phases-8-17.md) for commands and verification limits.
