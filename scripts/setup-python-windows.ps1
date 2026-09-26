$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    & "$PSScriptRoot\setup-windows.ps1"
    if (-not (Test-Path -LiteralPath '.venv\Scripts\python.exe')) {
        python -m venv .venv
        if ($LASTEXITCODE -ne 0) { throw 'Python environment creation failed.' }
    }
    $env:CMAKE_GENERATOR = 'Ninja'
    $env:CMAKE_BUILD_PARALLEL_LEVEL = '2'
    & .\.venv\Scripts\python.exe -m pip install -r requirements.lock
    if ($LASTEXITCODE -ne 0) { throw 'Python dependency installation failed.' }
    & .\.venv\Scripts\python.exe -m pip install . --no-build-isolation --no-deps
    if ($LASTEXITCODE -ne 0) { throw 'Python extension build failed.' }
    & .\.venv\Scripts\python.exe -c "import orderbook; print('C++ Python extension ready:', orderbook.MatchingEngine().healthy)"
    if ($LASTEXITCODE -ne 0) { throw 'Python extension import failed.' }
} finally {
    Pop-Location
}
