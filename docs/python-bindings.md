# Phase 8: Python bindings

`engine/src/bindings.cpp` exposes the existing C++ engine, enums, read-only order,
trade, level, and execution snapshots. `python/orderbook/__init__.py` loads the
extension and, on Windows, its bundled runtime directory. `pyproject.toml` uses
scikit-build-core to build a wheel through CMake, with `ORDERBOOK_BUILD_PYTHON=ON`
and C++ tests disabled during wheel builds. The standalone core remains independent.

pybind11 was selected because it maps typed C++ classes, STL containers, optional
values, and exceptions into Python without introducing a second matching algorithm.
This follows the [official CMake integration](https://pybind11.readthedocs.io/en/stable/compiling.html).
A hand-written C API would require more reference-count and conversion code;
moving matching to Python would abandon the intended core architecture.

Timestamps cross the binding as signed integer Unix microseconds, avoiding implicit
local-time conversion. The API supplies UTC values and serializes them to ISO 8601.
Book and trade queries copy data; Python cannot mutate internal queues or records.
Conversion adds O(number of returned records) time/space, beyond core complexity.
The GIL stays held, and the API adds a lock over the larger persistence boundary.
Calling the standalone C++ object concurrently remains unsupported.

Validation errors become Python ValueError, numeric overflow becomes OverflowError,
and duplicate/unknown/inactive orders have distinct Python exception classes.
Request type/width conversions are checked before entering the core. API schemas
apply stricter business validation, including rejection of booleans as quantities.

## Build and verification

```powershell
& .\scripts\setup-python-windows.ps1
& .\.venv\Scripts\python.exe -m pytest tests/test_bindings.py -q
```

Expected: three passing binding tests. Verified with CPython 3.14.3 x64 and
LLVM-MinGW Clang 21.1.1. The wheel includes `libc++.dll` and `libunwind.dll` so import
works without adding the compiler directory to PATH. This is a tested local
configuration, not a claim that MinGW is universally supported by pybind11.
The [upstream basics guide](https://pybind11.readthedocs.io/en/latest/basics.html)
lists Visual Studio as its supported Windows toolchain.

Ninja is explicitly selected by the Windows setup script. Optional pybind11 LTO
and stripping extras are disabled: its nested Windows strip command did not quote
this workspace's `&` correctly. The extension remains a Release build.

After editing C++, rebuild/reinstall the wheel. API source is importable from the
repository root, while the C++ module resides in the virtual environment. Normal
Linux builds use the same CMake target with a native compiler and Python headers.

Explain in an interview: Why return copies? Why keep the GIL? What would break if
the binding released it without another lock? Why must a wheel match Python's ABI?
