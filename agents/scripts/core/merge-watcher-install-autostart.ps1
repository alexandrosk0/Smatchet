# agents/scripts/core/merge-watcher-install-autostart.ps1
# Last remaining PowerShell file - see docs/harness/SETUP.md section Windows-only shims.
# ----------------------------------------------------------------------------
# Register a Windows Scheduled Task that starts `merge-watcher.py daemon` at
# user login. Phase 4c of `docs/plans/shipped/smatchet-merge-watcher.md`.
#
# Idempotent -- re-running unregisters + re-registers, so updates to the
# arguments (e.g. switching poll interval) take effect on next run.
#
# Limited run-level (no UAC prompt; daemon runs as current user). Restart on
# crash: up to 3 times, 5 min apart. Log file:
#   %LOCALAPPDATA%\Smatchet\merge-watch\daemon.log
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File agents/scripts/core/merge-watcher-install-autostart.ps1
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
    [string]$TaskName = "SmatchetMergeWatcher",
    # Skip the post-install start + health check (e.g. installing for a
    # different user session, or in CI where Task Scheduler can't start).
    [switch]$SkipStartCheck
)

$ErrorActionPreference = "Stop"

# Resolve repo root from script location (this file lives at
# <repo>/agents/scripts/core/merge-watcher-install-autostart.ps1).
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $scriptDir))
$daemonScript = Join-Path $scriptDir "merge-watcher.py"

if (-not (Test-Path $daemonScript)) {
    Write-Error "merge-watcher.py not found at expected path: $daemonScript"
    exit 1
}

# --- Daemon runtime prerequisites -------------------------------------------
# The Scheduled Task spawns python with a MINIMAL inherited env (user PATH
# only). The daemon shells out to bash + gh + jq via merge-gates.sh; if any
# of those aren't discoverable at task-start time, polls fail silently with
# empty status lines. Fail at INSTALL time rather than at first-poll-after-
# user-logout - much easier to fix in front of the screen.
$requiredTools = @(
    @{ name = "gh.exe"; install = "winget install GitHub.cli" },
    @{ name = "jq.exe"; install = "winget install jqlang.jq" },
    @{ name = "bash.exe"; install = "Git for Windows (https://git-scm.com/download/win)" }
)
$missing = @()
foreach ($t in $requiredTools) {
    $found = Get-Command $t.name -ErrorAction SilentlyContinue
    if (-not $found) {
        # Probe winget's Links dir + Git's known location since they're often
        # on user PATH but not Scheduled-Task PATH.
        $extraPaths = @(
            (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links\$($t.name)"),
            (Join-Path "$env:ProgramFiles\Git\bin" $t.name),
            (Join-Path "$env:ProgramFiles\GitHub CLI" $t.name)
        )
        $alt = $extraPaths | Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($alt) {
            Write-Host "  [OK] $($t.name) -> $alt (not on PATH; daemon will resolve via _resolve_gh_bin)" -ForegroundColor Yellow
        } else {
            $missing += "$($t.name) -- install: $($t.install)"
        }
    } else {
        Write-Host "  [OK] $($t.name) -> $($found.Source)" -ForegroundColor Green
    }
}
if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "Daemon prerequisites missing:" -ForegroundColor Red
    foreach ($m in $missing) { Write-Host "  - $m" -ForegroundColor Red }
    Write-Host ""
    Write-Host "Install the missing tools, restart your shell to pick up PATH, then re-run this script."
    Write-Host "Same set is checked at orchestrator setup time by: bash scripts/dev/check-required-tools.sh"
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
# ExecutionTimeLimit of TimeSpan::Zero = no time limit (daemon runs indefinitely).
$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -RestartCount 3 `
    -RestartInterval (New-TimeSpan -Minutes 5) `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -MultipleInstances IgnoreNew

Register-ScheduledTask `
    -TaskName $TaskName `
    -Action $action `
    -Trigger $trigger `
    -Settings $settings `
    -RunLevel Limited `
    -Description "Smatchet merge-watcher daemon -- polls registered PRs + auto-merges per docs/plans/shipped/smatchet-merge-watcher.md" | Out-Null

Write-Host ""
Write-Host "[OK] Scheduled Task '$TaskName' registered." -ForegroundColor Green
Write-Host "  Repo:           $repoRoot"
Write-Host "  Daemon script:  $daemonScript"
Write-Host "  Python:         $PythonExe"
Write-Host "  Poll interval:  $PollInterval seconds"
Write-Host "  Log file:       $logFile"
Write-Host ""

# --- Post-install verification (merge-watcher-liveness-unmonitored fix 1) ---
# Registration proves nothing about the ACTION: a stale path, missing
# interpreter, or bad quoting installs "successfully" and the first evidence
# is a poll that never happens - observed dead for days with 2 PRs registered
# (LastTaskResult=1 on every trigger, zero log output). So: start the task
# now and assert it SURVIVES. A broken action exits within ~1 s
# (LastTaskResult non-zero); a healthy daemon keeps running indefinitely.
if ($SkipStartCheck) {
    Write-Host "Post-install start check SKIPPED (-SkipStartCheck)." -ForegroundColor Yellow
    Write-Host "Daemon will start on next login. Start + verify manually with:"
    Write-Host "  Start-ScheduledTask -TaskName $TaskName"
    Write-Host "  Get-ScheduledTaskInfo -TaskName $TaskName   # LastTaskResult"
} else {
    Write-Host "Starting '$TaskName' to verify the registered action actually runs..."
    $preLogLen = 0
    if (Test-Path $logFile) { $preLogLen = (Get-Item $logFile).Length }
    Start-ScheduledTask -TaskName $TaskName
    # Healthy = Running on 3 CONSECUTIVE 2 s polls (~6 s sustained), not one
    # sample: a daemon that starts and crashes a few seconds in would pass a
    # single-sample check (CR review, PR #2007). A broken action never shows
    # Running at all; a crash-looper resets the streak.
    $healthy = $false
    $runStreak = 0
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        $state = (Get-ScheduledTask -TaskName $TaskName).State
        if ($state -eq "Running") {
            $runStreak++
            if ($runStreak -ge 3) {
                $healthy = $true
                break
            }
            continue
        }
        $runStreak = 0
        # Not running: either still Queued/starting up, or already exited.
        $info = Get-ScheduledTaskInfo -TaskName $TaskName
        if ($state -eq "Ready" -and $info.LastTaskResult -ne 0 -and $info.LastTaskResult -ne 267009) {
            break  # exited non-zero - fail fast with diagnostics below
        }
    }
    if ($healthy) {
        Write-Host "[OK] Daemon task is RUNNING (survived the start check)." -ForegroundColor Green
        if ((Test-Path $logFile) -and ((Get-Item $logFile).Length -gt $preLogLen)) {
            Write-Host "  Log is growing: $logFile"
        }
    } else {
        $info = Get-ScheduledTaskInfo -TaskName $TaskName
        Write-Host ""
        Write-Host "[FAIL] '$TaskName' did not stay running after start." -ForegroundColor Red
        Write-Host "  State:          $((Get-ScheduledTask -TaskName $TaskName).State)"
        Write-Host "  LastTaskResult: $($info.LastTaskResult)"
        Write-Host "  Action:         cmd.exe $cmdArgs"
        if (Test-Path $logFile) {
            Write-Host "  Log tail ($logFile):"
            Get-Content $logFile -Tail 10 | ForEach-Object { Write-Host "    $_" }
        } else {
            Write-Host "  Log file was never created: $logFile"
        }
        Write-Host "The task is registered but WILL NOT poll. Fix the action and re-run this installer."
        exit 1
    }
}
Write-Host ""
Write-Host "Check / control:"
Write-Host "  Get-ScheduledTask -TaskName $TaskName"
Write-Host "  Stop-ScheduledTask -TaskName $TaskName"
Write-Host "  Get-Content $logFile -Tail 20"
Write-Host ""
Write-Host "Uninstall:"
Write-Host "  powershell -ExecutionPolicy Bypass -File $scriptDir\merge-watcher-uninstall-autostart.ps1"
