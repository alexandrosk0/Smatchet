<#.
    Configure, build, and run SmatchetStandalone using the ninja-debug preset.
    Thin shim over build_and_run.ps1 -Preset ninja-debug.

    Examples:
      .\scripts\build_and_run_ninja_debug.ps1
      .\scripts\build_and_run_ninja_debug.ps1 -StandaloneArgs '--config','C:\tmp\config.json'
#>
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$StandaloneArgs = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$buildAndRun = Join-Path $PSScriptRoot "build_and_run.ps1"
& $buildAndRun -Preset "ninja-debug" -StandaloneArgs $StandaloneArgs
