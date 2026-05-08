<#.
    Configure, build, and run SmatchetStandalone using the vs-release preset
    (Visual Studio 2022 generator).
    Thin shim over build_and_run.ps1 -Preset vs-release.

    Examples:
      .\scripts\build_and_run_vs_release.ps1
      .\scripts\build_and_run_vs_release.ps1 -StandaloneArgs '--config','C:\tmp\config.json'
#>
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$StandaloneArgs = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$buildAndRun = Join-Path $PSScriptRoot "build_and_run.ps1"
& $buildAndRun -Preset "vs-release" -StandaloneArgs $StandaloneArgs
