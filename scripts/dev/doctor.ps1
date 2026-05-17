#requires -Version 5.1
<#
.SYNOPSIS
    Smatchet toolchain pre-flight check.

.DESCRIPTION
    Verifies every prerequisite for `cmake --preset ninja-iter-msys2` and
    `cmake --preset ninja-test-msys2` is on PATH and at a compatible version
    BEFORE the user runs CMake. Prints actionable install hints on failure
    instead of CMake's compiler-test diagnostics.

    Required checks (exit 1 if any fail):
        - cmake >= 3.24
        - ninja present
        - git present
        - MSYS2 UCRT64 gcc/g++ (>= 13) -- "toolchain reachable" check
        - lld / ld.lld present
        - python >= 3.10
        - >= 4 GB free under <repo>\build

    Warn-only checks (exit 2 if only these fail):
        - PATH contains C:\msys64\ucrt64\bin -- "on PATH at shell time".
          Required for scripts/dev/ bash scripts that resolve gcc via $PATH
          directly. NOT required for `cmake --preset` (presets prepend the
          UCRT64 bin via `MSYSTEM_PREFIX` internally). False-failed on hosts
          with JetBrains-bundled MinGW or other working-but-non-canonical
          gcc sources -- hence warn-only.
        - cppcheck
        - clang-tidy
        - clang-format

.OUTPUTS
    [PASS] <name> -- <version>
    [FAIL] <name> -- <install hint>
    [WARN] <name> -- <install hint>

    Final line:
        Doctor: GREEN   (all checks pass)
        Doctor: YELLOW  (only warn-only failures)
        Doctor: RED     (one or more required checks failed)

.NOTES
    Companion bash port: scripts/dev/doctor.sh.
    Verification: scripts/dev/test-doctor.sh.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Continue'

# Resolve repo root from this script's path: scripts/dev/doctor.ps1 -> <repo>
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = (Resolve-Path (Join-Path $scriptDir '..\..')).Path

$script:FailCount = 0
$script:WarnCount = 0

function Write-Pass([string]$Name, [string]$Detail) {
    Write-Host "[PASS] $Name -- $Detail" -ForegroundColor Green
}

function Write-Fail([string]$Name, [string]$Hint) {
    Write-Host "[FAIL] $Name -- $Hint" -ForegroundColor Red
    $script:FailCount++
}

function Write-Warn2([string]$Name, [string]$Hint) {
    Write-Host "[WARN] $Name -- $Hint" -ForegroundColor Yellow
    $script:WarnCount++
}

# Get the first line of `<cmd> --version` output, or $null if the command is missing.
function Get-ToolVersion([string]$Command, [string[]]$VersionArgs = @('--version')) {
    $exe = Get-Command $Command -ErrorAction SilentlyContinue
    if (-not $exe) { return $null }
    try {
        $output = & $exe.Source @VersionArgs 2>&1 | Out-String
        if ([string]::IsNullOrWhiteSpace($output)) { return $null }
        return ($output -split "`r?`n")[0].Trim()
    } catch {
        return $null
    }
}

# Parse the first "X.Y[.Z]" version tuple from a string. Returns [version] or $null.
function Parse-Version([string]$Text) {
    if (-not $Text) { return $null }
    $m = [regex]::Match($Text, '(\d+)\.(\d+)(?:\.(\d+))?')
    if (-not $m.Success) { return $null }
    $major = [int]$m.Groups[1].Value
    $minor = [int]$m.Groups[2].Value
    $patch = if ($m.Groups[3].Success) { [int]$m.Groups[3].Value } else { 0 }
    return [version]::new($major, $minor, $patch)
}

Write-Host "Smatchet doctor -- repo: $repoRoot"
Write-Host ""

# ----------------------------------------------------------------------------
# Required checks
# ----------------------------------------------------------------------------

# cmake >= 3.24
$cmakeLine = Get-ToolVersion 'cmake'
$cmakeVer  = Parse-Version $cmakeLine
if (-not $cmakeVer) {
    Write-Fail 'cmake' 'install: winget install Kitware.CMake'
} elseif ($cmakeVer -lt [version]'3.24.0') {
    Write-Fail 'cmake' "found $cmakeVer, need >= 3.24 -- upgrade: winget install Kitware.CMake"
} else {
    Write-Pass 'cmake' $cmakeLine
}

# ninja present
$ninjaLine = Get-ToolVersion 'ninja'
if (-not $ninjaLine) {
    Write-Fail 'ninja' 'install via MSYS2: pacman -S mingw-w64-ucrt-x86_64-ninja (ships with mingw-w64-ucrt-x86_64-toolchain)'
} else {
    Write-Pass 'ninja' "ninja $ninjaLine"
}

# git present
$gitLine = Get-ToolVersion 'git'
if (-not $gitLine) {
    Write-Fail 'git' 'install: winget install Git.Git'
} else {
    Write-Pass 'git' $gitLine
}

# gcc (>= 13) -- "toolchain reachable". Accept any gcc that resolves on
# PATH at version >= 13 (canonical MSYS2 UCRT64, JetBrains-bundled MinGW,
# or other working MinGW). `cmake --preset` will internally fix PATH to
# the UCRT64 prefix regardless. The "MSYS2 UCRT64 bin on PATH" check
# below is warn-only.
$ucrtBin = 'C:\msys64\ucrt64\bin'
$gccLine = Get-ToolVersion 'gcc'
$gccVer  = Parse-Version $gccLine
if (-not $gccVer) {
    Write-Fail 'gcc' "no gcc on PATH -- install: winget install MSYS2.MSYS2 then pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-lld"
} elseif ($gccVer.Major -lt 13) {
    Write-Fail 'gcc' "found gcc $gccVer, need >= 13 -- update: pacman -Syu then pacman -S mingw-w64-ucrt-x86_64-toolchain"
} else {
    $gccCmd = Get-Command gcc -ErrorAction SilentlyContinue
    $origin = if ($gccCmd -and $gccCmd.Source -like "*ucrt64*") { " (under $ucrtBin)" } elseif ($gccCmd) { " (resolves to $($gccCmd.Source))" } else { "" }
    Write-Pass 'gcc' "$gccLine$origin"
}

# g++ present (same toolchain)
$gxxLine = Get-ToolVersion 'g++'
if (-not $gxxLine) {
    Write-Fail 'g++' 'install via MSYS2: pacman -S mingw-w64-ucrt-x86_64-toolchain'
} else {
    Write-Pass 'g++' $gxxLine
}

# lld present (used by ninja-iter-msys2 + ninja-debug-msys2)
$lldLine = Get-ToolVersion 'lld'
if (-not $lldLine) {
    $lldLine = Get-ToolVersion 'ld.lld'
}
if (-not $lldLine) {
    Write-Fail 'lld' 'install via MSYS2: pacman -S mingw-w64-ucrt-x86_64-lld (required by ninja-iter-msys2 linker step)'
} else {
    Write-Pass 'lld' $lldLine
}

# python >= 3.10
$pyLine = Get-ToolVersion 'python'
$pyVer  = Parse-Version $pyLine
if (-not $pyVer) {
    Write-Fail 'python' 'install: winget install Python.Python.3.12'
} elseif ($pyVer -lt [version]'3.10.0') {
    Write-Fail 'python' "found $pyVer, need >= 3.10 -- upgrade: winget install Python.Python.3.12"
} else {
    Write-Pass 'python' $pyLine
}

# Disk: >= 4 GB free under <repo>\build (FetchContent + LTO needs space)
$buildDir = Join-Path $repoRoot 'build'
$diskTarget = if (Test-Path -LiteralPath $buildDir) { $buildDir } else { $repoRoot }
try {
    $driveLetter = (Split-Path -Qualifier (Resolve-Path -LiteralPath $diskTarget).Path).TrimEnd(':')
    $drive = Get-PSDrive -Name $driveLetter -ErrorAction Stop
    $freeGb = [math]::Round($drive.Free / 1GB, 1)
    if ($freeGb -lt 4.0) {
        Write-Fail 'disk' "only $freeGb GB free on $driveLetter`: -- need >= 4 GB for FetchContent _deps + LTO objects"
    } else {
        Write-Pass 'disk' "$freeGb GB free on $driveLetter`: (target $diskTarget)"
    }
} catch {
    Write-Fail 'disk' "could not check free space at $diskTarget -- $_"
}

# ----------------------------------------------------------------------------
# Warn-only checks
# ----------------------------------------------------------------------------

# PATH contains C:\msys64\ucrt64\bin -- warn-only because `cmake --preset
# ninja-iter-msys2` prepends the UCRT64 prefix via `MSYSTEM_PREFIX` regardless.
# This check matters for scripts/dev/ bash wrappers that resolve gcc via $PATH
# directly. False-failed on JetBrains-bundled-MinGW hosts pre-split.
$pathParts = $env:PATH -split ';' | ForEach-Object { $_.TrimEnd('\') }
$ucrtBinNormalized = $ucrtBin.TrimEnd('\')
$pathHasUcrt = $false
foreach ($p in $pathParts) {
    if ($p -ieq $ucrtBinNormalized) { $pathHasUcrt = $true; break }
}
if (-not $pathHasUcrt) {
    Write-Warn2 'PATH' "MSYS2 UCRT64 bin ($ucrtBin) not on PATH -- prepend it for direct-gcc invocation by scripts/dev/ (not required for cmake --preset)"
} else {
    Write-Pass 'PATH' "contains $ucrtBin"
}

$cppcheckLine = Get-ToolVersion 'cppcheck'
if (-not $cppcheckLine) {
    Write-Warn2 'cppcheck' 'install via MSYS2: pacman -S mingw-w64-ucrt-x86_64-cppcheck (used by scripts/dev/run_cppcheck.py)'
} else {
    Write-Pass 'cppcheck' $cppcheckLine
}

$clangTidyLine = Get-ToolVersion 'clang-tidy'
if (-not $clangTidyLine) {
    Write-Warn2 'clang-tidy' 'install via MSYS2: pacman -S mingw-w64-ucrt-x86_64-clang-tools-extra (used by lint hook)'
} else {
    Write-Pass 'clang-tidy' $clangTidyLine
}

$clangFmtLine = Get-ToolVersion 'clang-format'
if (-not $clangFmtLine) {
    Write-Warn2 'clang-format' 'install via MSYS2: pacman -S mingw-w64-ucrt-x86_64-clang-tools-extra (used by lint hook)'
} else {
    Write-Pass 'clang-format' $clangFmtLine
}

# Opt-in: OpenCppCoverage check is skipped by default. Set the env var
# SMATCHET_DOCTOR_CHECK_COVERAGE=1 to enable. Windows-only; install via
# `choco install opencppcoverage` or the releases page. CI runners install
# it themselves, so this check is only relevant for local coverage runs.
if ($env:SMATCHET_DOCTOR_CHECK_COVERAGE -eq '1') {
    $occLine = Get-ToolVersion 'OpenCppCoverage' --help
    if (-not $occLine) {
        Write-Warn2 'OpenCppCoverage' 'install via Chocolatey (choco install opencppcoverage) or releases page (used by scripts/dev/coverage.sh; CI installs it via Chocolatey, so this is only relevant for local coverage runs)'
    } else {
        Write-Pass 'OpenCppCoverage' $occLine
    }
}

# ----------------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------------

Write-Host ""
if ($script:FailCount -gt 0) {
    Write-Host "Doctor: RED ($script:FailCount required failure(s), $script:WarnCount warning(s))" -ForegroundColor Red
    exit 1
} elseif ($script:WarnCount -gt 0) {
    Write-Host "Doctor: YELLOW ($script:WarnCount warning(s) -- build works, optional tools missing)" -ForegroundColor Yellow
    exit 2
} else {
    Write-Host "Doctor: GREEN (all checks passed)" -ForegroundColor Green
    exit 0
}
