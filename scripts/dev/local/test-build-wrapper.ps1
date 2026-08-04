<#.
    Lightweight smoke tests for build.ps1, build_and_run.ps1 and run_standalone.ps1.
    No actual CMake build is triggered — tests use fake exes and mock inputs, so
    they run on any machine (and in CI) with no compiler installed.

    Tests covered:
      1. Passing -Preset ninja-iter-msys2 to build_standalone.ps1 exits with
         non-zero and prints the migration hint.
      2. run_standalone.ps1 output includes exe path + timestamp for the
         selected exe.
      3. Stale sibling table is formatted correctly (>=2 existing siblings).
      4. build.ps1 preset auto-detect table — one row per compiler-availability
         branch (cl.exe / clang-cl.exe only / neither), asserting both the printed
         preset and the printed routing reason.
      5. build.ps1 explicit -Preset wins over auto-detect, and -Target / -BuildOnly /
         -RunOnly / -Verify / -StandaloneArgs all bind by name in the delegate even
         when routed through with-msvc.ps1's positional splat.
      6. build.ps1 invokes build_and_run.ps1 directly (no with-msvc.ps1) when
         cl.exe is already on PATH.
      7. build.ps1 maps with-msvc.ps1's toolchain-missing status (78) to install
         hints and propagates it — while a wrapped command that merely exited 2
         propagates untouched, with no bogus "no usable MSVC toolchain" claim.
         Build Tools is stated as mandatory; the LLVM hint appears only for a
         clang preset, as an addition rather than an alternative.
      8. build_standalone.ps1 fails fast on every msvc/clang preset with no cl.exe,
         before reaching the cmake configure step — including the mid-name-token
         presets (ninja-msvc-asan, ninja-clang-asan, *-msvc-arm64).
      9. The pre-flight does not over-reach: presets with no msvc/clang token
         (ninja-tsan-linux, ninja-test-linux) never demand cl.exe.
     10. A clang preset with cl.exe present but no clang-cl.exe still fails fast,
         naming clang-cl, before reaching the cmake configure step.

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
# Helpers for the build.ps1 dispatcher tests (4-7).
#
# build.ps1 resolves both delegates relative to $PSScriptRoot, so a sandbox holding
# a copy of build.ps1 plus stub delegates exercises every dispatch branch with no
# compiler, no CMake configure and no real vcvars import. Which branch is taken is
# decided purely by what the child process finds on PATH.
# ──────────────────────────────────────────────────────────────────────────────
function New-BuildPs1Sandbox {
    param(
        [switch]$WithCl,
        [switch]$WithClangCl
    )
    $sandbox = Join-Path $env:TEMP "smatchet-buildps1-$([System.IO.Path]::GetRandomFileName())"
    New-Item -ItemType Directory -Path (Join-Path $sandbox "scripts\dev\local") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $sandbox "bin") -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repoRoot "build.ps1") -Destination (Join-Path $sandbox "build.ps1")

    # Mirrors the real build_and_run.ps1 param block, and echoes what actually bound,
    # so a forwarding regression shows up as a wrong value rather than a silent pass.
    $buildAndRunStub = @(
        'param(',
        '    [string]$Preset = "",',
        '    [string]$Target = "",',
        '    [switch]$BuildOnly,',
        '    [switch]$RunOnly,',
        '    [switch]$Verify,',
        '    [Parameter(ValueFromRemainingArguments = $true)][string[]]$StandaloneArgs = @()',
        ')',
        'Write-Host "STUB build_and_run: Preset=$Preset Target=$Target BuildOnly=$BuildOnly RunOnly=$RunOnly Verify=$Verify StandaloneArgs=$StandaloneArgs"',
        'exit 0'
    )
    Set-Content -Encoding ASCII -Path (Join-Path $sandbox "scripts\dev\local\build_and_run.ps1") -Value $buildAndRunStub

    # Mirrors with-msvc.ps1's real signature and its `& $exe @rest` tail (:135-139) --
    # the positional splat is exactly what build.ps1 has to route around, so the stub
    # reproduces it instead of shortcutting to a plain echo.
    $withMsvcStub = @(
        'param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Command)',
        'Write-Host "STUB with-msvc: $Command"',
        'if ($env:SMATCHET_TEST_WITHMSVC_EXIT) { exit [int]$env:SMATCHET_TEST_WITHMSVC_EXIT }',
        '$exe = $Command[0]',
        '$rest = @()',
        'if ($Command.Count -gt 1) { $rest = $Command[1..($Command.Count - 1)] }',
        '& $exe @rest',
        'exit $LASTEXITCODE'
    )
    Set-Content -Encoding ASCII -Path (Join-Path $sandbox "scripts\dev\with-msvc.ps1") -Value $withMsvcStub

    if ($WithCl)      { Set-Content -Encoding ASCII -Path (Join-Path $sandbox "bin\cl.exe")       -Value "@echo off`r`nexit 0" }
    if ($WithClangCl) { Set-Content -Encoding ASCII -Path (Join-Path $sandbox "bin\clang-cl.exe") -Value "@echo off`r`nexit 0" }
    return $sandbox
}

function Invoke-BuildPs1Sandbox {
    param(
        [Parameter(Mandatory = $true)][string]$Sandbox,
        [string]$Arguments = "",
        [string]$WithMsvcExit = ""
    )
    # powershell.exe lives in System32\WindowsPowerShell\v1.0 (NOT System32 itself), and
    # build.ps1 shells out to it by name -- so that directory has to be on the child PATH.
    # None of these supply cl.exe / clang-cl.exe, so the sandbox bin\ directory alone
    # decides which compiler the dispatcher can see.
    $childPath = "$Sandbox\bin;$env:SystemRoot\System32\WindowsPowerShell\v1.0;$env:SystemRoot\System32;$env:SystemRoot"
    # `exit $LASTEXITCODE` at the top level of -Command is what makes build.ps1's own exit
    # code the child process's exit code; a nested `& script.ps1` exit alone does not.
    $script = "`$env:PATH = '$childPath'; " +
              "`$env:SMATCHET_TEST_WITHMSVC_EXIT = '$WithMsvcExit'; " +
              "& '$Sandbox\build.ps1' $Arguments 2>&1; " +
              "exit `$LASTEXITCODE"

    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = powershell -NoProfile -ExecutionPolicy Bypass -Command $script 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $prevEap

    return [pscustomobject]@{
        Output   = (($output | ForEach-Object { "$_" }) -join "`n")
        ExitCode = $exitCode
    }
}

# ──────────────────────────────────────────────────────────────────────────────
# Test 4: build.ps1 preset auto-detect table, one row per compiler-availability branch.
# ──────────────────────────────────────────────────────────────────────────────
Invoke-Test "build.ps1: preset auto-detect table (cl / clang-cl / neither)" {
    $cases = @(
        @{ Name = "cl.exe present";      Cl = $true;  ClangCl = $false; Preset = "ninja-iter-msvc";  Reason = "cl.exe already on PATH" },
        @{ Name = "clang-cl only";       Cl = $false; ClangCl = $true;  Preset = "ninja-iter-clang"; Reason = "no cl.exe on PATH" },
        @{ Name = "no compiler on PATH"; Cl = $false; ClangCl = $false; Preset = "ninja-iter-msvc";  Reason = "no cl.exe on PATH" }
    )
    foreach ($case in $cases) {
        $sandbox = New-BuildPs1Sandbox -WithCl:$case.Cl -WithClangCl:$case.ClangCl
        try {
            $run = Invoke-BuildPs1Sandbox -Sandbox $sandbox -Arguments "-BuildOnly"
            if ($Verbose) { Write-Host "  [$($case.Name)] exit=$($run.ExitCode)`n$($run.Output)" }

            if ($run.Output -notmatch ("preset {0} \(auto-detected\)" -f [regex]::Escape($case.Preset))) {
                throw "[$($case.Name)] expected 'preset $($case.Preset) (auto-detected)'. Got:`n$($run.Output)"
            }
            if ($run.Output -notmatch [regex]::Escape($case.Reason)) {
                throw "[$($case.Name)] expected routing reason '$($case.Reason)'. Got:`n$($run.Output)"
            }
            if ($run.Output -notmatch ("Preset={0} " -f [regex]::Escape($case.Preset))) {
                throw "[$($case.Name)] expected the preset to reach build_and_run.ps1 named-bound. Got:`n$($run.Output)"
            }
        }
        finally { Remove-Item -Recurse -Force -LiteralPath $sandbox -ErrorAction SilentlyContinue }
    }
}

# ──────────────────────────────────────────────────────────────────────────────
# Test 5: explicit -Preset wins over auto-detect, and every switch forwards intact.
# ──────────────────────────────────────────────────────────────────────────────
Invoke-Test "build.ps1: explicit -Preset overrides auto-detect and forwards switches" {
    # No cl.exe, so this also proves named arguments survive the with-msvc splat.
    $sandbox = New-BuildPs1Sandbox
    try {
        $run = Invoke-BuildPs1Sandbox -Sandbox $sandbox -Arguments "-Preset ninja-debug-msvc -BuildOnly -StandaloneArgs --version"
        if ($Verbose) { Write-Host "  exit=$($run.ExitCode)`n$($run.Output)" }

        if ($run.Output -notmatch "preset ninja-debug-msvc \(explicit -Preset overrides auto-detect\)") {
            throw "Expected the explicit-preset reason line. Got:`n$($run.Output)"
        }
        if ($run.Output -match "auto-detected") {
            throw "Explicit -Preset must not report auto-detection. Got:`n$($run.Output)"
        }
        if ($run.Output -notmatch "Preset=ninja-debug-msvc Target=SmatchetStandalone BuildOnly=True RunOnly=False Verify=False StandaloneArgs=--version") {
            throw "Expected every forwarded argument to bind by name. Got:`n$($run.Output)"
        }
    }
    finally { Remove-Item -Recurse -Force -LiteralPath $sandbox -ErrorAction SilentlyContinue }
}

# ──────────────────────────────────────────────────────────────────────────────
# Test 6: with cl.exe already on PATH, build_and_run.ps1 is invoked directly.
# ──────────────────────────────────────────────────────────────────────────────
Invoke-Test "build.ps1: cl.exe on PATH bypasses with-msvc.ps1" {
    $sandbox = New-BuildPs1Sandbox -WithCl
    try {
        $run = Invoke-BuildPs1Sandbox -Sandbox $sandbox -Arguments "-BuildOnly"
        if ($Verbose) { Write-Host "  exit=$($run.ExitCode)`n$($run.Output)" }

        if ($run.Output -notmatch "STUB build_and_run:") {
            throw "Expected build_and_run.ps1 to be invoked. Got:`n$($run.Output)"
        }
        if ($run.Output -match "STUB with-msvc:") {
            throw "with-msvc.ps1 must be skipped when cl.exe is already on PATH. Got:`n$($run.Output)"
        }
        if ($run.ExitCode -ne 0) {
            throw "Expected exit 0 on the direct path, got $($run.ExitCode)."
        }
    }
    finally { Remove-Item -Recurse -Force -LiteralPath $sandbox -ErrorAction SilentlyContinue }
}

# ──────────────────────────────────────────────────────────────────────────────
# Test 7: with-msvc.ps1's toolchain-missing status (78) surfaces install hints —
#         and nothing else does. Row 3 is the regression that motivated the
#         dedicated code: with-msvc propagates the wrapped command's exit code,
#         so a build that legitimately exited 2 used to be reported as a missing
#         MSVC install. Rows 1-2 pin the hint text: Build Tools is mandatory for
#         every Windows preset, so LLVM is offered only for a clang preset and
#         only as an addition to it.
# ──────────────────────────────────────────────────────────────────────────────
Invoke-Test "build.ps1: with-msvc exit 78 surfaces install hints; a propagated exit 2 does not" {
    $cases = @(
        @{ Name = "no toolchain, msvc preset";  Exit = 78; Arguments = "-BuildOnly";                          WantHints = $true;  WantLlvm = $false },
        @{ Name = "no toolchain, clang preset"; Exit = 78; Arguments = "-Preset ninja-iter-clang -BuildOnly"; WantHints = $true;  WantLlvm = $true  },
        @{ Name = "wrapped build exited 2";     Exit = 2;  Arguments = "-BuildOnly";                          WantHints = $false; WantLlvm = $false }
    )
    foreach ($case in $cases) {
        $sandbox = New-BuildPs1Sandbox
        try {
            $run = Invoke-BuildPs1Sandbox -Sandbox $sandbox -Arguments $case.Arguments -WithMsvcExit "$($case.Exit)"
            if ($Verbose) { Write-Host "  [$($case.Name)] exit=$($run.ExitCode)`n$($run.Output)" }

            if ($run.ExitCode -ne $case.Exit) {
                throw "[$($case.Name)] expected with-msvc's exit $($case.Exit) to propagate, got $($run.ExitCode). Output:`n$($run.Output)"
            }

            $sawDiagnosis = [bool]($run.Output -match "no usable MSVC toolchain")
            if ($case.WantHints -ne $sawDiagnosis) {
                if ($case.WantHints) {
                    throw "[$($case.Name)] expected the no-toolchain diagnosis. Got:`n$($run.Output)"
                }
                throw "[$($case.Name)] a propagated child exit code must not be reported as a missing toolchain. Got:`n$($run.Output)"
            }

            if ($case.WantHints) {
                if ($run.Output -notmatch [regex]::Escape("Install Visual Studio Build Tools with the C++ workload")) {
                    throw "[$($case.Name)] Build Tools must be stated as mandatory, not one option among several. Got:`n$($run.Output)"
                }
                if ($run.Output -notmatch [regex]::Escape("winget install Microsoft.VisualStudio.2022.BuildTools")) {
                    throw "[$($case.Name)] expected the Build Tools winget hint. Got:`n$($run.Output)"
                }
            }

            $sawLlvm = [bool]($run.Output -match [regex]::Escape("winget install LLVM.LLVM"))
            if ($case.WantLlvm -ne $sawLlvm) {
                if ($case.WantLlvm) {
                    throw "[$($case.Name)] expected the LLVM hint for a clang preset. Got:`n$($run.Output)"
                }
                throw "[$($case.Name)] LLVM cannot substitute for the MSVC environment, so the hint must not appear here. Got:`n$($run.Output)"
            }
        }
        finally { Remove-Item -Recurse -Force -LiteralPath $sandbox -ErrorAction SilentlyContinue }
    }
}

# ──────────────────────────────────────────────────────────────────────────────
# Test 8: build_standalone.ps1 fails fast on every MSVC-needing preset with no
#         cl.exe, before reaching the cmake configure step.
#
#         The table is not just ninja-iter-msvc: the toolchain token sits mid-name
#         in the sanitizer and arm64 presets, which a "*-msvc" suffix test silently
#         skipped. Every preset here must trip the pre-flight.
# ──────────────────────────────────────────────────────────────────────────────
$MsvcNeedingPresets = @(
    "ninja-iter-msvc",          # suffix form — the original case
    "ninja-msvc-asan",          # token mid-name
    "ninja-clang-asan",         # token mid-name, clang-cl still needs the MSVC env
    "ninja-iter-msvc-arm64",    # token mid-name, arm64 suffix
    "ninja-publish-msvc-arm64"
)

Invoke-Test "build_standalone: every msvc/clang preset without cl.exe fails fast before cmake" {
    # cmake + ninja must resolve so their Assert-Command checks pass and the
    # pre-flight is what actually fires; neither fake is ever executed.
    $fakeBin = Join-Path $env:TEMP "smatchet-fakebin-$([System.IO.Path]::GetRandomFileName())"
    New-Item -ItemType Directory -Path $fakeBin | Out-Null
    foreach ($tool in @("cmake.exe", "ninja.exe")) {
        Set-Content -Encoding ASCII -Path (Join-Path $fakeBin $tool) -Value "@echo off`r`nexit 0"
    }

    try {
        $buildScript = Join-Path $PSScriptDir "build_standalone.ps1"
        $childPath = "$fakeBin;$env:SystemRoot\System32;$env:SystemRoot"
        foreach ($preset in $MsvcNeedingPresets) {
            $prevEap = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            $output = powershell -NoProfile -ExecutionPolicy Bypass `
                -Command "`$env:PATH = '$childPath'; & '$buildScript' -Preset $preset 2>&1" 2>&1
            $exitCode = $LASTEXITCODE
            $ErrorActionPreference = $prevEap
            $combined = ($output | ForEach-Object { "$_" }) -join "`n"
            if ($Verbose) { Write-Host "  $preset -> exit=$exitCode  output=$combined" }

            if ($exitCode -eq 0) {
                throw "[$preset] Expected non-zero exit when cl.exe is missing, got 0. Output:`n$combined"
            }
            if ($combined -notmatch "cl\.exe is not on PATH") {
                throw "[$preset] Expected the missing-cl.exe diagnosis. Got:`n$combined"
            }
            if ($combined -notmatch [regex]::Escape("build.ps1")) {
                throw "[$preset] Expected the message to name .\build.ps1. Got:`n$combined"
            }
            if ($combined -match "cmake --preset") {
                throw "[$preset] Pre-flight must fire before the cmake configure step. Got:`n$combined"
            }
        }
    }
    finally { Remove-Item -Recurse -Force -LiteralPath $fakeBin -ErrorAction SilentlyContinue }
}

# ──────────────────────────────────────────────────────────────────────────────
# Test 9: the widened match must not over-reach — a preset that carries no
#         msvc/clang token never demands cl.exe.
# ──────────────────────────────────────────────────────────────────────────────
Invoke-Test "build_standalone: non-MSVC presets are not caught by the cl.exe pre-flight" {
    $fakeBin = Join-Path $env:TEMP "smatchet-fakebin-$([System.IO.Path]::GetRandomFileName())"
    New-Item -ItemType Directory -Path $fakeBin | Out-Null
    foreach ($tool in @("cmake.exe", "ninja.exe")) {
        Set-Content -Encoding ASCII -Path (Join-Path $fakeBin $tool) -Value "@echo off`r`nexit 0"
    }

    try {
        $buildScript = Join-Path $PSScriptDir "build_standalone.ps1"
        $childPath = "$fakeBin;$env:SystemRoot\System32;$env:SystemRoot"
        foreach ($preset in @("ninja-tsan-linux", "ninja-test-linux")) {
            $prevEap = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            $output = powershell -NoProfile -ExecutionPolicy Bypass `
                -Command "`$env:PATH = '$childPath'; & '$buildScript' -Preset $preset 2>&1" 2>&1
            $ErrorActionPreference = $prevEap
            $combined = ($output | ForEach-Object { "$_" }) -join "`n"
            if ($Verbose) { Write-Host "  $preset -> output=$combined" }

            if ($combined -match "cl\.exe is not on PATH") {
                throw "[$preset] carries no msvc/clang token and must not trip the pre-flight. Got:`n$combined"
            }
        }
    }
    finally { Remove-Item -Recurse -Force -LiteralPath $fakeBin -ErrorAction SilentlyContinue }
}

# ──────────────────────────────────────────────────────────────────────────────
# Test 10: cl.exe alone does not satisfy a clang preset. The clang presets pin
#          CMAKE_CXX_COMPILER to clang-cl, and the MSVC environment only ships
#          one when the "C++ Clang tools for Windows" component is installed —
#          so a host with Build Tools but no LLVM must be told about clang-cl
#          rather than dying later inside cmake on an opaque compiler error.
# ──────────────────────────────────────────────────────────────────────────────
Invoke-Test "build_standalone: a clang preset with cl.exe but no clang-cl.exe fails fast before cmake" {
    $fakeBin = Join-Path $env:TEMP "smatchet-fakebin-$([System.IO.Path]::GetRandomFileName())"
    New-Item -ItemType Directory -Path $fakeBin | Out-Null
    # cl.exe present on purpose: it clears the earlier MSVC-environment gate, so the
    # clang-cl gate is the only thing left that can fire. None of the fakes are executed.
    foreach ($tool in @("cmake.exe", "ninja.exe", "cl.exe")) {
        Set-Content -Encoding ASCII -Path (Join-Path $fakeBin $tool) -Value "@echo off`r`nexit 0"
    }

    try {
        $buildScript = Join-Path $PSScriptDir "build_standalone.ps1"
        $childPath = "$fakeBin;$env:SystemRoot\System32;$env:SystemRoot"
        foreach ($preset in @("ninja-iter-clang", "ninja-clang-asan")) {
            $prevEap = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            $output = powershell -NoProfile -ExecutionPolicy Bypass `
                -Command "`$env:PATH = '$childPath'; & '$buildScript' -Preset $preset 2>&1" 2>&1
            $exitCode = $LASTEXITCODE
            $ErrorActionPreference = $prevEap
            $combined = ($output | ForEach-Object { "$_" }) -join "`n"
            if ($Verbose) { Write-Host "  $preset -> exit=$exitCode  output=$combined" }

            if ($exitCode -eq 0) {
                throw "[$preset] Expected non-zero exit when clang-cl.exe is missing, got 0. Output:`n$combined"
            }
            if ($combined -notmatch "clang-cl\.exe is not on PATH") {
                throw "[$preset] Expected the missing-clang-cl diagnosis. Got:`n$combined"
            }
            # Lookbehind, not a bare "cl\.exe": the clang-cl message contains the cl.exe
            # message as a substring, so an unanchored match would flag every run.
            if ($combined -match "(?<!clang-)cl\.exe is not on PATH") {
                throw "[$preset] cl.exe IS on PATH here; the earlier gate must not fire. Got:`n$combined"
            }
            if ($combined -match "cmake --preset") {
                throw "[$preset] Pre-flight must fire before the cmake configure step. Got:`n$combined"
            }
        }
    }
    finally { Remove-Item -Recurse -Force -LiteralPath $fakeBin -ErrorAction SilentlyContinue }
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
