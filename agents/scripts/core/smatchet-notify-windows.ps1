# agents/scripts/core/smatchet-notify-windows.ps1
# Last remaining PowerShell file - see docs/harness/SETUP.md section Windows-only shims.
# ----------------------------------------------------------------------------
# Windows native toast notification for smatchet-merge-watcher Phase 4a.
# Invoked by agents/scripts/core/smatchet-notify.sh on Windows when the Smatchet
# in-app toast endpoint is unavailable (Smatchet closed, or Phase 4b not
# shipped yet).
#
# Requires the BurntToast module. One-shot opt-in install + self-test:
#   powershell -ExecutionPolicy Bypass -File agents/scripts/core/merge-watcher-notify-setup.ps1
#
# Exits 0 on toast dispatched; 1 if BurntToast missing or toast call failed.
#
# ASCII-only on purpose: Windows PowerShell 5.1 reads this no-BOM file as ANSI
# and mis-decodes non-ASCII bytes. A literal em-dash or astral emoji in code or
# a string breaks the parser (observed: an em-dash in the title string + astral
# [char] casts silently killed every toast). Emoji are emitted at runtime via
# [char]::ConvertFromUtf32 (handles code points above U+FFFF), never as literal
# glyphs or [char] casts (which overflow on astral code points).
# ----------------------------------------------------------------------------

param(
    [Parameter(Mandatory=$true)][int]$PR,
    [Parameter(Mandatory=$true)][string]$State,
    [Parameter(Mandatory=$true)][string]$Message,
    [string]$PrUrl = ""
)

$ErrorActionPreference = "Stop"

# BurntToast presence check -- no auto-install (user opt-in only). The one-shot
# opt-in installer is agents/scripts/core/merge-watcher-notify-setup.ps1.
$module = Get-Module -ListAvailable -Name BurntToast
if (-not $module) {
    Write-Error "BurntToast module not installed. Run the one-shot setup: powershell -ExecutionPolicy Bypass -File agents/scripts/core/merge-watcher-notify-setup.ps1  (or: Install-Module BurntToast -Scope CurrentUser)"
    exit 1
}

Import-Module BurntToast -ErrorAction Stop

# Per-state emoji code point. ConvertFromUtf32 (not [char]) so astral points
# (U+1F4AC etc.) become a valid surrogate-pair string instead of overflowing.
$stateCodePoint = @{
    "CI_FAIL"                  = 0x274C   # cross mark
    "CONFLICT"                 = 0x26A0   # warning sign
    "USER_COMMENT"             = 0x1F4AC  # speech balloon
    "TIMEOUT_NO_CR"            = 0x23F0   # alarm clock
    "TRIAGE_BUDGET_EXHAUSTED"  = 0x1F6D1  # stop sign
}[$State]
if (-not $stateCodePoint) { $stateCodePoint = 0x1F514 }  # bell (default)
$stateEmoji = [System.Char]::ConvertFromUtf32($stateCodePoint)

$title = "$stateEmoji  Smatchet watcher: PR #$PR - $State"
$body = $Message
if ([string]::IsNullOrEmpty($body)) { $body = "(no message)" }

$toastArgs = @{
    Text = @($title, $body)
    AppLogo = $null
    UniqueIdentifier = "smatchet-merge-watcher-pr-$PR"
}
if (-not [string]::IsNullOrEmpty($PrUrl)) {
    $toastArgs.Button = New-BTButton -Content "Open PR" -Arguments $PrUrl
}

try {
    New-BurntToastNotification @toastArgs
    exit 0
} catch {
    Write-Error "BurntToast notification failed: $_"
    exit 1
}
