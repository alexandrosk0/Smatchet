<#
.SYNOPSIS
  Launch Smatchet (or any exe) under Sysinternals procdump so a crash that bypasses
  Windows Error Reporting is still captured as a full minidump.

.DESCRIPTION
  Some Smatchet crashes manifest as an "instant vanish" with NO WER dump: the frame-loop
  SEH filter (Source/Standalone/SmatchetCrashHandler.cpp) catches the fault and calls
  std::exit(), which is a deliberate ExitProcess — never an "unhandled" exception WER can
  see. Running the target as a child of procdump captures it anyway: -e dumps on an
  unhandled exception, -t dumps on (abnormal) termination, -ma writes a full dump.

  Pair with first-chance breaking under cdb when you need the PRIMARY faulting stack (the
  app's own SEH filter swallows the access violation before WER/procdump's -e sees it):
    cdb -y "<build-dir>;srv*C:\Tools\symcache*https://msdl.microsoft.com/download/symbols" `
        -cf scripts\dev\av-capture.cdb <exe>
  where av-capture.cdb does: sxe -c "kP; .dump /ma <out.dmp>; q" av ; g

  procdump itself: https://download.sysinternals.com/files/Procdump.zip
  Machine-wide auto-capture for EVERY future crash (one-time, elevated):
    procdump64.exe -accepteula -ma -i <dump-dir>

.PARAMETER Exe
  Path to the executable. Defaults to the standalone iter build.

.PARAMETER DumpDir
  Where procdump writes dumps. Defaults to %LOCALAPPDATA%\CrashDumps.

.PARAMETER Procdump
  Path to procdump64.exe. Defaults to C:\Tools\Procdump\procdump64.exe.

.PARAMETER AutocyclePanes
  When set (frame interval, e.g. 20), exports SMATCHET_AUTOCYCLE_PANES so a debug build
  auto-cycles the focused grid pane every N frames — hands-free reproduction of the
  rapid-backend-switch class of crashes. Requires a build that honours the env var.

.EXAMPLE
  pwsh scripts\dev\run-with-procdump.ps1
.EXAMPLE
  pwsh scripts\dev\run-with-procdump.ps1 -AutocyclePanes 20
#>
[CmdletBinding()]
param(
    [string]$Exe = "$PSScriptRoot\..\..\build\ninja-iter-msvc\Smatchet.exe",
    [string]$DumpDir = "$env:LOCALAPPDATA\CrashDumps",
    [string]$Procdump = 'C:\Tools\Procdump\procdump64.exe',
    [int]$AutocyclePanes = 0
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Exe)) { throw "Executable not found: $Exe" }
if (-not (Test-Path $Procdump)) {
    throw "procdump not found at $Procdump. Get it: https://download.sysinternals.com/files/Procdump.zip (extract procdump64.exe to C:\Tools\Procdump)."
}
New-Item -ItemType Directory -Force -Path $DumpDir | Out-Null

$exeFull = (Resolve-Path $Exe).Path
$workDir = Split-Path $exeFull
if ($AutocyclePanes -gt 0) { $env:SMATCHET_AUTOCYCLE_PANES = "$AutocyclePanes" }

# -ma full dump, -e on unhandled exception, -t on termination, -x launches + monitors.
$pdArgs = @('-accepteula', '-ma', '-e', '-t', '-x', $DumpDir, $exeFull)
Write-Host "Launching under procdump:`n  $Procdump $($pdArgs -join ' ')" -ForegroundColor Cyan
Write-Host "Dumps -> $DumpDir" -ForegroundColor Cyan
& $Procdump @pdArgs
