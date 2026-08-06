# agents/scripts/core/merge-watcher-install-prune-task.ps1
# Last remaining PowerShell file - see docs/harness/SETUP.md section Windows-only shims.
# ----------------------------------------------------------------------------
# Register a Windows Scheduled Task that runs `merge-watch prune` daily, so the
# watcher registry self-heals even across daemon-down windows.
#
# Why a separate scheduled task (not just the daemon's reconcile-on-poll):
# the daemon can only reconcile PRs while it is RUNNING. If it crashes, the
# machine is logged out, or it is simply stopped for a while, merged/closed
# PRs accrete in the registry and slow every live PR's poll cadence. This
# daily sweep heals the registry independently of the daemon's lifecycle.
# `prune` is fail-safe - it KEEPS any PR whose state it can't determine.
#
# Idempotent -- re-running unregisters + re-registers, so changes to the
# schedule take effect on next run.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File agents/scripts/core/merge-watcher-install-prune-task.ps1
#
# Optional parameters:
#   -At <HH:mm>        Daily run time (default 09:15).
#   -PythonExe <path>  Override the Python interpreter (default: `python` on PATH).
#   -TaskName <name>   Override "SmatchetMergeWatcherPrune".
#
# Uninstall:
#   Unregister-ScheduledTask -TaskName SmatchetMergeWatcherPrune -Confirm:$false
# ----------------------------------------------------------------------------

param(
    [string]$At = "09:15",
    [string]$PythonExe = "",
    [string]$TaskName = "SmatchetMergeWatcherPrune"
)

$ErrorActionPreference = "Stop"

# Validate -At is a strict 24-hour HH:mm so a typo fails here with a clear
# message instead of deferring to New-ScheduledTaskTrigger (CR #534).
if ($At -notmatch '^([01]\d|2[0-3]):[0-5]\d$') {
    Write-Error "-At must be 24-hour HH:mm (e.g. 09:15); got '$At'."
    exit 1
}

# Resolve repo root from script location (this file lives at
# <repo>/agents/scripts/core/merge-watcher-install-prune-task.ps1).
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $scriptDir))
$cliScript = Join-Path $scriptDir "merge-watcher-cli.py"

if (-not (Test-Path $cliScript)) {
    Write-Error "merge-watcher-cli.py not found at expected path: $cliScript"
    exit 1
}

# Resolve Python -- explicit param wins, else first `python` on PATH.
if ([string]::IsNullOrEmpty($PythonExe)) {
    $pythonCmd = Get-Command python -ErrorAction SilentlyContinue
    if (-not $pythonCmd) {
        Write-Error "No 'python' on PATH. Install Python 3 or pass -PythonExe <path>."
        exit 1
    }
    $PythonExe = $pythonCmd.Source
}
if (-not (Test-Path $PythonExe)) {
    Write-Error "Python interpreter not found at: $PythonExe"
    exit 1
}

# Log dir matches the daemon's watcher_root() resolution.
if ([string]::IsNullOrEmpty($env:LOCALAPPDATA)) {
    Write-Error "LOCALAPPDATA is not set; cannot resolve the watcher log dir (matches merge-watcher-cli.py watcher_root)."
    exit 1
}
$logDir = Join-Path $env:LOCALAPPDATA "Smatchet\merge-watch"
if (-not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}
$logFile = Join-Path $logDir "prune.log"

# Wrap the python invocation in cmd.exe so stdout/stderr redirect into the log
# file (Scheduled-task Action takes Execute + Argument only). Append (>>) so the
# janitor keeps a rolling history rather than truncating each run.
$cmdArgs = "/c `"`"$PythonExe`" `"$cliScript`" prune >> `"$logFile`" 2>&1`""

# Unregister any existing task with this name to keep install idempotent.
$existing = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "Unregistering existing '$TaskName' before re-installing..."
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
}

$action = New-ScheduledTaskAction `
    -Execute "cmd.exe" `
    -Argument $cmdArgs `
    -WorkingDirectory $repoRoot

# Daily trigger at the requested local time. StartWhenAvailable catches up if
# the machine was asleep/off at the scheduled instant.
$trigger = New-ScheduledTaskTrigger -Daily -At $At

# Short-lived task: cap the run at 10 min (prune is a handful of gh calls) so a
# wedged gh can't leave it hanging. No restart-on-crash - it just runs again
# tomorrow.
$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 10) `
    -MultipleInstances IgnoreNew

Register-ScheduledTask `
    -TaskName $TaskName `
    -Action $action `
    -Trigger $trigger `
    -Settings $settings `
    -RunLevel Limited `
    -Description "Smatchet merge-watcher registry janitor -- daily `merge-watch prune` to unregister merged/closed PRs. See agents/scripts/core/merge-watcher-cli.py." | Out-Null

Write-Host ""
Write-Host "[OK] Scheduled Task '$TaskName' registered." -ForegroundColor Green
Write-Host "  Repo:          $repoRoot"
Write-Host "  CLI script:    $cliScript"
Write-Host "  Python:        $PythonExe"
Write-Host "  Runs daily at: $At"
Write-Host "  Log file:      $logFile"
Write-Host ""
Write-Host "Run now (sanity check):"
Write-Host "  Start-ScheduledTask -TaskName $TaskName"
Write-Host "  python `"$cliScript`" prune --dry-run    # preview without mutating"
Write-Host ""
Write-Host "Uninstall:"
Write-Host "  Unregister-ScheduledTask -TaskName $TaskName -Confirm:`$false"
