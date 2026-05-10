<#.
    Configure, build, and run a Smatchet target.

    Examples:
      .\scripts\build_and_run.ps1
      .\scripts\build_and_run.ps1 -Preset ninja-iter-msys2
      .\scripts\build_and_run.ps1 -Preset ninja-debug-unreal-msys2 -Target SmatchetPackageUnrealLibs_DX12
      .\scripts\build_and_run.ps1 -BuildOnly
      .\scripts\build_and_run.ps1 -RunOnly -StandaloneArgs '--config','C:\tmp\config.json'

    For just-build or just-run workflows, you can also call
    build_standalone.ps1 / run_standalone.ps1 directly.
#>
param(
    [string]$Preset = "ninja-debug-msys2",
    [string]$Target = "SmatchetStandalone",
    [switch]$BuildOnly,
    [switch]$RunOnly,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$StandaloneArgs = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ($BuildOnly -and $RunOnly) {
    throw "Cannot specify both -BuildOnly and -RunOnly."
}

$buildScript = Join-Path $PSScriptRoot "build_standalone.ps1"
$runScript = Join-Path $PSScriptRoot "run_standalone.ps1"

if (-not $RunOnly) {
    & $buildScript -Preset $Preset -Target $Target
}

if (-not $BuildOnly) {
    # Pass exe args by name to avoid positional misbinding in run_standalone.ps1
    # (its [string]$ExeName / $BuildDir / $Configuration would otherwise grab
    # leading splatted values).
    & $runScript -Preset $Preset -Target $Target -StandaloneArgs $StandaloneArgs
}
