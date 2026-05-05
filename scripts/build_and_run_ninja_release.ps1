<#.
    Configure (cmake --preset ninja-release), build SmatchetStandalone, then run it.

    From repo root:
      .\scripts\build_and_run_ninja_release.ps1
      .\scripts\build_and_run_ninja_release.ps1 -- extra args forwarded to SmatchetStandalone
#>
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$StandaloneArgs = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build/ninja-release"

Push-Location $repoRoot
try {
    cmake --preset ninja-release
    cmake --build --preset ninja-release --target SmatchetStandalone
}
finally {
    Pop-Location
}

$exe = Join-Path $buildDir "SmatchetStandalone.exe"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Expected executable missing: $exe"
}

if ($StandaloneArgs.Count -gt 0) {
    & $exe @StandaloneArgs
}
else {
    & $exe
}
