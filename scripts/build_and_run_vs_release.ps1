<#.
    Configure, build, and run SmatchetStandalone using the ninja-publish-msys2 preset.
    Thin shim over build_and_run.ps1 -Preset ninja-publish-msys2.

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
& $buildAndRun -Preset "ninja-publish-msys2" -StandaloneArgs $StandaloneArgs
