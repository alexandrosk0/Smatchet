<#.
    Configure, build, and run SmatchetStandalone using the vs-debug preset
    (Visual Studio 2022 generator).
    Thin shim over build_and_run.ps1 -Preset vs-debug.

    Examples:
      .\scripts\build_and_run_vs_debug.ps1
      .\scripts\build_and_run_vs_debug.ps1 -StandaloneArgs '--config','C:\tmp\config.json'
#>
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$StandaloneArgs = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$buildAndRun = Join-Path $PSScriptRoot "build_and_run.ps1"
& $buildAndRun -Preset "vs-debug" -StandaloneArgs $StandaloneArgs
