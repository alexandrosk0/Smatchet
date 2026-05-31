<#.
    Lightweight smoke tests for build_and_run.ps1 and run_standalone.ps1.
    No actual CMake build is triggered — tests use fake exes and mock inputs.

    Tests covered:
      1. Passing -Preset ninja-iter-msys2 to build_standalone.ps1 exits with
         non-zero and prints the migration hint.
      2. run_standalone.ps1 output includes exe path + timestamp for the
         selected exe.
      3. Stale sibling table is formatted correctly (>=2 existing siblings).

    Exit codes:
      0 — all tests passed
      1 — one or more tests failed
#>
param(
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$PSScriptDir = $PSScriptRoot
$repoRoot    = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptDir))

$passed  = 0
$failed  = 0
$results = @()

function Invoke-Test {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )
    $result = "PASS"
    $detail = ""
    try {
        & $Body
    }
    catch {
        $result = "FAIL"
        $detail = $_.Exception.Message
    }
    $script:results += [pscustomobject]@{ Name = $Name; Result = $result; Detail = $detail }
    if ($result -eq "PASS") { $script:passed++ } else { $script:failed++ }
}

# ──────────────────────────────────────────────────────────────────────────────
# Test 1: Retired msys2 preset exits non-zero with migration hint.
# ──────────────────────────────────────────────────────────────────────────────
Invoke-Test "build_standalone: ninja-iter-msys2 rejected with migration hint" {
    $buildScript = Join-Path $PSScriptDir "build_standalone.ps1"
    # Run in a child process so ErrorActionPreference = Stop doesn't terminate us.
    # Suppress error records from the child process exit so only $LASTEXITCODE is used.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = powershell -NoProfile -ExecutionPolicy Bypass `
        -Command "& '$buildScript' -Preset ninja-iter-msys2 2>&1" 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $previous

    if ($Verbose) { Write-Host "  exit=$exitCode  output=$output" }

    if ($exitCode -eq 0) {
        throw "Expected non-zero exit for retired msys2 preset, got 0."
    }
    $combined = ($output | ForEach-Object { "$_" }) -join " "
    if ($combined -notmatch "retired") {
        throw "Expected 'retired' in output. Got: $combined"
    }
    if ($combined -notmatch "ninja-iter-msvc") {
        throw "Expected 'ninja-iter-msvc' migration hint in output. Got: $combined"
    }
}

# ──────────────────────────────────────────────────────────────────────────────
# Test 2: run_standalone.ps1 prints exe path + timestamp for selected exe.
# ──────────────────────────────────────────────────────────────────────────────
Invoke-Test "run_standalone: prints exe path and LastWriteTime" {
    # Create a temporary fake exe so the script can find it.
    $tempDir = Join-Path $env:TEMP "smatchet-test-$([System.IO.Path]::GetRandomFileName())"
    New-Item -ItemType Directory -Path $tempDir | Out-Null
    $fakeExe = Join-Path $tempDir "Smatchet.exe"
    # Write a minimal batch script that immediately exits so Start-Process or
    # foreground launch doesn't hang if the test accidentally reaches launch.
    Set-Content -Path $fakeExe -Value "@echo off`r`nexit 0" -Encoding ASCII

    try {
        $runScript = Join-Path $PSScriptDir "run_standalone.ps1"
        # Run with -BuildDir pointing to our fake dir and a dummy arg so it takes
        # the foreground code path (command-style), meaning it will call & $exe
        # and return quickly.  The key is that path/time output happens BEFORE
        # launch, so we capture it from stdout.
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $output = powershell -NoProfile -ExecutionPolicy Bypass `
            -Command "& '$runScript' -BuildDir '$tempDir' -StandaloneArgs '--version' 2>&1" 2>&1
        $ErrorActionPreference = $prevEap
        $combined = ($output | ForEach-Object { "$_" }) -join "`n"
        if ($Verbose) { Write-Host "  output=$combined" }

        if ($combined -notmatch [regex]::Escape($fakeExe)) {
            throw "Expected exe path '$fakeExe' in output. Got:`n$combined"
        }
        if ($combined -notmatch "Time\s*:") {
            throw "Expected 'Time :' timestamp line in output. Got:`n$combined"
        }
    }
    finally {
        Remove-Item -Recurse -Force -LiteralPath $tempDir -ErrorAction SilentlyContinue
    }
}

# ──────────────────────────────────────────────────────────────────────────────
# Test 3: Stale sibling table appears when >=2 sibling exes exist.
# ──────────────────────────────────────────────────────────────────────────────
Invoke-Test "run_standalone: stale sibling comparison table formatted correctly" {
    # Create two fake sibling dirs under build/ so the sibling scan finds them.
    $buildRoot = Join-Path $repoRoot "build"
    $siblingPresets = @("ninja-debug-msvc", "ninja-iter-msvc")
    $createdDirs  = @()
    $createdFiles = @()
    foreach ($preset in $siblingPresets) {
        $dir = Join-Path $buildRoot $preset
        if (-not (Test-Path -LiteralPath $dir)) {
            New-Item -ItemType Directory -Path $dir | Out-Null
            $createdDirs += $dir
        }
        $sibExe = Join-Path $dir "Smatchet.exe"
        if (-not (Test-Path -LiteralPath $sibExe)) {
            Set-Content -Path $sibExe -Value "@echo off`r`nexit 0" -Encoding ASCII
            $createdFiles += $sibExe
        }
    }

    # Selected exe is in a distinct temp dir (not one of the siblings).
    $tempDir = Join-Path $env:TEMP "smatchet-test-$([System.IO.Path]::GetRandomFileName())"
    New-Item -ItemType Directory -Path $tempDir | Out-Null
    $fakeExe = Join-Path $tempDir "Smatchet.exe"
    Set-Content -Path $fakeExe -Value "@echo off`r`nexit 0" -Encoding ASCII

    try {
        $runScript = Join-Path $PSScriptDir "run_standalone.ps1"
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $output = powershell -NoProfile -ExecutionPolicy Bypass `
            -Command "& '$runScript' -BuildDir '$tempDir' -StandaloneArgs '--version' 2>&1" 2>&1
        $ErrorActionPreference = $prevEap
        $combined = ($output | ForEach-Object { "$_" }) -join "`n"
        if ($Verbose) { Write-Host "  output=$combined" }

        if ($combined -notmatch "Sibling exe comparison") {
            throw "Expected 'Sibling exe comparison' header in output. Got:`n$combined"
        }
        if ($combined -notmatch "<<< selected") {
            throw "Expected '<<< selected' marker in output. Got:`n$combined"
        }
    }
    finally {
        Remove-Item -Recurse -Force -LiteralPath $tempDir -ErrorAction SilentlyContinue
        # Remove only what we created; files first so dirs are empty when deleted.
        foreach ($f in $createdFiles) {
            Remove-Item -Force -LiteralPath $f -ErrorAction SilentlyContinue
        }
        foreach ($d in $createdDirs) {
            Remove-Item -Recurse -Force -LiteralPath $d -ErrorAction SilentlyContinue
        }
    }
}

# ──────────────────────────────────────────────────────────────────────────────
# Report
# ──────────────────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "Test results:"
foreach ($r in $results) {
    $tag = if ($r.Result -eq "PASS") { "[PASS]" } else { "[FAIL]" }
    Write-Host ("  {0}  {1}" -f $tag, $r.Name)
    if ($r.Detail) {
        Write-Host ("         {0}" -f $r.Detail)
    }
}
Write-Host ""
Write-Host ("{0} passed, {1} failed." -f $passed, $failed)

if ($failed -gt 0) {
    exit 1
}
exit 0
