# agents/scripts/core/merge-watcher-uninstall-autostart.ps1
# Last remaining PowerShell file - see docs/harness/SETUP.md section Windows-only shims.
# ----------------------------------------------------------------------------
# Counterpart to merge-watcher-install-autostart.ps1. Removes the Scheduled
# Task + stops the running daemon (if any). Phase 4c of
# docs/plans/shipped/smatchet-merge-watcher.md.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File agents/scripts/core/merge-watcher-uninstall-autostart.ps1
#
# Idempotent -- silently succeeds if the task doesn't exist.
#
# Does NOT remove the daemon log file or the active.json registry. Those are
# considered user data (they survive uninstall by design -- re-installing
# resumes watching the same PR set).
# ----------------------------------------------------------------------------

param(
    [string]$TaskName = "SmatchetMergeWatcher"
)

$ErrorActionPreference = "Stop"

$existing = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if (-not $existing) {
    Write-Host "merge-watcher: no Scheduled Task '$TaskName' found; nothing to uninstall."
    exit 0
}

# Stop the running instance first (if any) so the unregister doesn't race a
# live daemon writing to active.json.
$state = (Get-ScheduledTask -TaskName $TaskName).State
if ($state -eq "Running") {
    Write-Host "Stopping running '$TaskName' daemon..."
    Stop-ScheduledTask -TaskName $TaskName
    Start-Sleep -Milliseconds 500
}

Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false

Write-Host "[OK] Scheduled Task '$TaskName' removed." -ForegroundColor Green
Write-Host ""
Write-Host "User-data preserved (re-install resumes the same registry):"
Write-Host "  Registry: $env:LOCALAPPDATA\Smatchet\merge-watch\active.json"
Write-Host "  Log:      $env:LOCALAPPDATA\Smatchet\merge-watch\daemon.log"
Write-Host ""
Write-Host "To purge user data:"
Write-Host "  Remove-Item -Recurse $env:LOCALAPPDATA\Smatchet\merge-watch"
