$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$repoRoot = Split-Path -Parent $PSScriptRoot
$toolsDir = Join-Path $repoRoot '.tools'
$pythonEnv = Join-Path $toolsDir 'python'
$pythonExe = Join-Path $pythonEnv 'Scripts\python.exe'
$toolchainName = 'llvm-mingw-20250910-ucrt-x86_64'
$compilerBin = Join-Path $toolsDir "$toolchainName\bin"
$compilerExe = Join-Path $compilerBin 'clang++.exe'

if (-not [Environment]::Is64BitOperatingSystem -or
    $env:PROCESSOR_ARCHITECTURE -ne 'AMD64') {
    throw 'This setup script requires x64 Windows and x64 PowerShell.'
}

New-Item -ItemType Directory -Path $toolsDir -Force | Out-Null

if (-not (Test-Path -LiteralPath $pythonExe)) {
    & python -m venv $pythonEnv
    if ($LASTEXITCODE -ne 0) { throw 'Creating the build-tools environment failed.' }
}

& $pythonExe -m pip install -r (Join-Path $repoRoot 'requirements-build.txt')
if ($LASTEXITCODE -ne 0) { throw 'Installing CMake and Ninja failed.' }

if (-not (Test-Path -LiteralPath $compilerExe)) {
    $archive = Join-Path $toolsDir "$toolchainName.zip"
    $url = "https://github.com/mstorsjo/llvm-mingw/releases/download/20250910/$toolchainName.zip"
    $expectedHash = 'bd88084d7a3b95906fa295453399015a1fdd7b90a38baa8f78244bd234303737'

    if (-not (Test-Path -LiteralPath $archive)) {
        Write-Host 'Downloading portable LLVM-MinGW (approximately 181 MB)...'
        Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) {
        throw "Compiler archive checksum mismatch. Remove '$archive' and rerun."
    }

    Write-Host 'Extracting the compiler...'
    Expand-Archive -LiteralPath $archive -DestinationPath $toolsDir -Force
}

# Changes apply to this PowerShell process, not the system or user PATH.
$env:Path = "$pythonEnv\Scripts;$compilerBin;$env:Path"
$env:CXX = $compilerExe

& $compilerExe --version
if ($LASTEXITCODE -ne 0) { throw 'The C++ compiler could not run.' }
& cmake --version
if ($LASTEXITCODE -ne 0) { throw 'CMake could not run.' }
& ninja --version
if ($LASTEXITCODE -ne 0) { throw 'Ninja could not run.' }
Write-Host 'Build tools are ready in this PowerShell session.'
