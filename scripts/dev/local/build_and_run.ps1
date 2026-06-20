<#.
    Configure, build, and run a Smatchet target.

    Examples:
      .\scripts\dev\build_and_run.ps1
      .\scripts\dev\build_and_run.ps1 -Preset ninja-iter-msvc
      .\scripts\dev\build_and_run.ps1 -Preset ninja-iter-unreal-msvc -Target SmatchetPackageUnrealLibs_DX12
      .\scripts\dev\build_and_run.ps1 -BuildOnly
      .\scripts\dev\build_and_run.ps1 -BuildOnly -Verify      # build then run the CI delta-lint gates
      .\scripts\dev\build_and_run.ps1 -RunOnly -StandaloneArgs '--config','C:\tmp\config.json'

    For just-build or just-run workflows, you can also call
    build_standalone.ps1 / run_standalone.ps1 directly. For build-then-delta-lint,
    scripts\dev\verify.ps1 wraps the same gates (-Verify here delegates to it).
#>
param(
    [string]$Preset = "ninja-iter-msvc",
    [string]$Target = "SmatchetStandalone",
    [switch]$BuildOnly,
    [switch]$RunOnly,
    [switch]$Verify,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$StandaloneArgs = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ($BuildOnly -and $RunOnly) {
    throw "Cannot specify both -BuildOnly and -RunOnly."
}
if ($Verify -and $RunOnly) {
    throw "Cannot specify both -Verify and -RunOnly (the delta-lint gates run after a build)."
}

$buildScript = Join-Path $PSScriptRoot "build_standalone.ps1"
$runScript = Join-Path $PSScriptRoot "run_standalone.ps1"

if (-not $RunOnly) {
    & $buildScript -Preset $Preset -Target $Target
}

# Build-verify shortcut no longer skips the delta-lint gates: -Verify runs the
# comment-noise + delta lint gates after a successful build (verify.ps1 -SkipBuild,
# since the build just ran). Closes the "build-green != lint-green" CI surprise.
if ($Verify) {
    $verifyScript = Join-Path $PSScriptRoot "..\verify.ps1"
    & $verifyScript -Preset $Preset -Target $Target -SkipBuild
}

if (-not $BuildOnly) {
    # Pass exe args by name to avoid positional misbinding in run_standalone.ps1
    # (its [string]$ExeName / $BuildDir / $Configuration would otherwise grab
    # leading splatted values).
    & $runScript -Preset $Preset -Target $Target -StandaloneArgs $StandaloneArgs
}
