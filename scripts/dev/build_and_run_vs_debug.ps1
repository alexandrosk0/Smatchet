<#.
    Configure, build, and run SmatchetStandalone using the ninja-debug-msys2 preset.
    Thin shim over build_and_run.ps1 -Preset ninja-debug-msys2.

    Examples:
      .\scripts\dev\build_and_run_vs_debug.ps1
      .\scripts\dev\build_and_run_vs_debug.ps1 -StandaloneArgs '--config','C:\tmp\config.json'
#>
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$StandaloneArgs = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$buildAndRun = Join-Path $PSScriptRoot "build_and_run.ps1"
& $buildAndRun -Preset "ninja-debug-msys2" -StandaloneArgs $StandaloneArgs
