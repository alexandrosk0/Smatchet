# Temporary build helper — MSVC ASAN preset
# Strips MSYS2/MinGW from PATH, loads vcvars64, then configures + builds.
param(
    [string]$SourceDir = "C:\Dev\Smatchet"
)

Set-Location $SourceDir

# Strip MSYS2/MinGW entries from PATH so cl.exe wins
$cleanPath = ($env:PATH -split ';') | Where-Object {
    $_ -notmatch 'msys|mingw|ucrt|MSYS2' -and $_ -notmatch '\\msys64\\'
}
$env:PATH = $cleanPath -join ';'

# Load vcvars64 environment via cmd /c
$vcvarsResult = cmd /c "`"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`" && set" 2>&1
foreach ($line in $vcvarsResult) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}

Write-Host "=== cl.exe version ===" -ForegroundColor Cyan
cl.exe 2>&1 | Select-Object -First 2

Write-Host "=== Configuring ninja-msvc-asan ===" -ForegroundColor Cyan
cmake --preset ninja-msvc-asan -DSMATCHET_ENABLE_STRICT_WARNINGS=ON
if ($LASTEXITCODE -ne 0) {
    Write-Error "Configure FAILED (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

Write-Host "=== Building ninja-msvc-asan ===" -ForegroundColor Cyan
cmake --build --preset ninja-msvc-asan 2>&1
exit $LASTEXITCODE
