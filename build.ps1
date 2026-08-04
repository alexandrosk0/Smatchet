<#.
    Blessed entry point - build (and run) Smatchet from an ordinary PowerShell.
    No build logic lives here: a thin dispatcher that picks a preset, prints the
    reason, and delegates to scripts/dev/local/build_and_run.ps1 - directly when
    cl.exe is on PATH, else through scripts/dev/with-msvc.ps1 (which imports the
    MSVC vcvars environment first; its exit 78 means "no usable VS install" -- a
    wrapper-level status, distinct from any code the wrapped build itself returns).

    Examples: .\build.ps1 | .\build.ps1 -BuildOnly
              .\build.ps1 -Preset ninja-debug-msvc -BuildOnly
#>
param(
    [string]$Preset,
    [string]$Target = "SmatchetStandalone",
    [switch]$BuildOnly,
    [switch]$RunOnly,
    [switch]$Verify,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$StandaloneArgs = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$buildAndRun = Join-Path $PSScriptRoot "scripts\dev\local\build_and_run.ps1"
$withMsvc    = Join-Path $PSScriptRoot "scripts\dev\with-msvc.ps1"

$haveCl = [bool](Get-Command "cl.exe" -ErrorAction SilentlyContinue)
$presetReason = "explicit -Preset overrides auto-detect"
if ([string]::IsNullOrWhiteSpace($Preset)) {
    if ($haveCl) { $Preset = "ninja-iter-msvc" }
    elseif (Get-Command "clang-cl.exe" -ErrorAction SilentlyContinue) { $Preset = "ninja-iter-clang" }
    else { $Preset = "ninja-iter-msvc" }
    $presetReason = "auto-detected"
}
Write-Host "build.ps1: preset $Preset ($presetReason)"

$forward = @("-Preset", $Preset, "-Target", $Target)
if ($BuildOnly) { $forward += "-BuildOnly" }
if ($RunOnly)   { $forward += "-RunOnly" }
if ($Verify)    { $forward += "-Verify" }
if ($StandaloneArgs.Count -gt 0) { $forward += @("-StandaloneArgs") + $StandaloneArgs }

# A PowerShell array splats POSITIONALLY, so `& $script @forward` (and with-msvc.ps1's own
# `& $exe @rest` tail) mangles -Preset/-Target; powershell.exe -File binds them by name.
$child = @("powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $buildAndRun) + $forward
$global:LASTEXITCODE = 0
if ($haveCl) {
    Write-Host "build.ps1: cl.exe already on PATH - invoking build_and_run.ps1 directly"
    & $child[0] @($child | Select-Object -Skip 1)
}
else {
    Write-Host "build.ps1: no cl.exe on PATH - importing the MSVC environment via with-msvc.ps1"
    & $withMsvc -Command $child
    # 78, not 2: with-msvc.ps1 propagates the wrapped command's own exit code, so a
    # build that happened to exit 2 would otherwise be misreported as "no toolchain".
    if ($LASTEXITCODE -eq 78) {
        # Build Tools is mandatory, not one option among several: every Windows preset
        # (clang included) needs the MSVC environment for the Windows SDK headers/libs,
        # and build_standalone.ps1's pre-flight demands cl.exe for all of them. LLVM
        # alone cannot satisfy this, so it is listed as an addition, never a substitute.
        Write-Host "build.ps1: no usable MSVC toolchain (see the with-msvc line above). Install Visual Studio Build Tools with the C++ workload:"
        Write-Host "  winget install Microsoft.VisualStudio.2022.BuildTools --override `"--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`"   # provides cl.exe"
        if ($Preset -like "*clang*") {
            Write-Host "  winget install LLVM.LLVM   # additionally required for $Preset (clang-cl), on top of the Build Tools above"
        }
    }
}
exit $LASTEXITCODE
