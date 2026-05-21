# scripts/dev/smatchet-notify-windows.ps1
# ----------------------------------------------------------------------------
# Windows native toast notification for smatchet-merge-watcher Phase 4a.
# Invoked by scripts/dev/smatchet-notify.sh on Windows when the Smatchet
# in-app toast endpoint is unavailable (Smatchet closed, or Phase 4b not
# shipped yet).
#
# Requires the BurntToast module. Install on first run:
#   Install-Module BurntToast -Scope CurrentUser
#
# Exits 0 on toast dispatched; 1 if BurntToast missing or toast call failed.
# ----------------------------------------------------------------------------

param(
    [Parameter(Mandatory=$true)][int]$PR,
    [Parameter(Mandatory=$true)][string]$State,
    [Parameter(Mandatory=$true)][string]$Message,
    [string]$PrUrl = ""
)

$ErrorActionPreference = "Stop"

# BurntToast presence check — no auto-install (user opt-in only).
$module = Get-Module -ListAvailable -Name BurntToast
if (-not $module) {
    Write-Error "BurntToast module not installed. Install with: Install-Module BurntToast -Scope CurrentUser"
    exit 1
}

Import-Module BurntToast -ErrorAction Stop

# Per-state emoji + AppId hint so notifications group sensibly in Action Center.
$stateEmoji = @{
    "CI_FAIL"                  = [char]0x274C  # ❌
    "CONFLICT"                 = [char]0x26A0  # ⚠
    "USER_COMMENT"             = [char]0x1F4AC # 💬
    "TIMEOUT_NO_CR"            = [char]0x23F0  # ⏰
    "TRIAGE_BUDGET_EXHAUSTED"  = [char]0x1F6D1 # 🛑
}[$State]
if (-not $stateEmoji) { $stateEmoji = [char]0x1F514 }  # 🔔 default

$title = "$stateEmoji  Smatchet watcher: PR #$PR — $State"
$body = $Message
if ([string]::IsNullOrEmpty($body)) { $body = "(no message)" }

$args = @{
    Text = @($title, $body)
    AppLogo = $null
    UniqueIdentifier = "smatchet-merge-watcher-pr-$PR"
}
if (-not [string]::IsNullOrEmpty($PrUrl)) {
    $args.Button = New-BTButton -Content "Open PR" -Arguments $PrUrl
}

try {
    New-BurntToastNotification @args
    exit 0
} catch {
    Write-Error "BurntToast notification failed: $_"
    exit 1
}
