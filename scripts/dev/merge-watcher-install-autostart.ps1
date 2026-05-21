# scripts/dev/merge-watcher-install-autostart.ps1
# ----------------------------------------------------------------------------
# Register a Windows Scheduled Task that starts `merge-watcher.py daemon` at
# user login. Phase 4c of `docs/design/smatchet-merge-watcher.md`.
#
# Idempotent -- re-running unregisters + re-registers, so updates to the
# arguments (e.g. switching poll interval) take effect on next run.
#
# Limited run-level (no UAC prompt; daemon runs as current user). Restart on
# crash: up to 3 times, 5 min apart. Log file:
#   %LOCALAPPDATA%\Smatchet\merge-watch\daemon.log
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts/dev/merge-watcher-install-autostart.ps1
#
# Optional parameters:
#   -PollInterval <seconds>  Override MERGE_WATCH_POLL_INTERVAL (default 60).
#   -PythonExe <path>        Override the Python interpreter. Defaults to
#                            the `python` resolved on PATH at install time.
#   -TaskName <name>         Override "SmatchetMergeWatcher".
# ----------------------------------------------------------------------------

param(
    [int]$PollInterval = 60,
    [string]$PythonExe = "",
    [string]$TaskName = "SmatchetMergeWatcher"
)

$ErrorActionPreference = "Stop"

# Resolve repo root from script location (this file lives at
# <repo>/scripts/dev/merge-watcher-install-autostart.ps1).
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
$daemonScript = Join-Path $scriptDir "merge-watcher.py"

if (-not (Test-Path $daemonScript)) {
    Write-Error "merge-watcher.py not found at expected path: $daemonScript"
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

# Resolve log dir (matches the daemon's `watcher_root()` resolution).
$logDir = Join-Path $env:LOCALAPPDATA "Smatchet\merge-watch"
if (-not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}
$logFile = Join-Path $logDir "daemon.log"

# Build the argument string. Wrap each path in double quotes for cmd.exe
# parsing inside Task Scheduler.
$daemonArgs = "`"$daemonScript`" daemon --poll-interval $PollInterval"

# Unregister any existing task with this name to keep install idempotent.
$existing = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "Unregistering existing '$TaskName' before re-installing..."
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
}

# Wrap python invocation in cmd.exe so the > redirect actually writes to the
# log file. Scheduled-task Action takes Execute + Argument; the cleanest way
# to get stdout redirection is to spawn cmd.exe /c "...".
$cmdArgs = "/c `"`"$PythonExe`" $daemonArgs > `"$logFile`" 2>&1`""

$action = New-ScheduledTaskAction `
    -Execute "cmd.exe" `
    -Argument $cmdArgs `
    -WorkingDirectory $repoRoot

# Trigger: at login of the user running this install. The task runs in this
# user's identity (RunLevel Limited = no admin token).
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME

# Restart-on-crash: up to 3 attempts, 5 min apart.
$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -RestartCount 3 `
    -RestartInterval (New-TimeSpan -Minutes 5) `
    -ExecutionTimeLimit ([TimeSpan]::Zero) ` # 0 = no time limit
    -MultipleInstances IgnoreNew

Register-ScheduledTask `
    -TaskName $TaskName `
    -Action $action `
    -Trigger $trigger `
    -Settings $settings `
    -RunLevel Limited `
    -Description "Smatchet merge-watcher daemon -- polls registered PRs + auto-merges per docs/design/smatchet-merge-watcher.md" | Out-Null

Write-Host ""
Write-Host "[OK] Scheduled Task '$TaskName' registered." -ForegroundColor Green
Write-Host "  Repo:           $repoRoot"
Write-Host "  Daemon script:  $daemonScript"
Write-Host "  Python:         $PythonExe"
Write-Host "  Poll interval:  $PollInterval seconds"
Write-Host "  Log file:       $logFile"
Write-Host ""
Write-Host "Daemon will start on next login. Start now with:"
Write-Host "  Start-ScheduledTask -TaskName $TaskName"
Write-Host ""
Write-Host "Check / control:"
Write-Host "  Get-ScheduledTask -TaskName $TaskName"
Write-Host "  Stop-ScheduledTask -TaskName $TaskName"
Write-Host "  Get-Content $logFile -Tail 20"
Write-Host ""
Write-Host "Uninstall:"
Write-Host "  powershell -ExecutionPolicy Bypass -File $scriptDir\merge-watcher-uninstall-autostart.ps1"
