# agents/scripts/core/merge-watcher-notify-setup.ps1
# Last remaining PowerShell file - see docs/harness/SETUP.md section Windows-only shims.
# ----------------------------------------------------------------------------
# One-shot setup for merge-watcher Windows desktop notifications.
#
# The watcher's notify dispatcher (agents/scripts/core/smatchet-notify.sh) tries a
# Windows native toast via smatchet-notify-windows.ps1, which needs the
# BurntToast PowerShell module. That ps1 deliberately does NOT auto-install
# (no silent module pulls from the hot notify path) -- THIS script is the
# explicit, user-run opt-in that installs it once and verifies it works.
#
# Without a working toast channel the watcher merges PRs SILENTLY (it falls
# back to the file-log channel only), so you never get told a PR merged or got
# stuck -- exactly the "I have to keep poking it" problem this fixes.
#
# What it does:
#   1. Installs BurntToast (CurrentUser scope) if missing.
#   2. Imports it + fires a test toast so you can confirm it reaches you.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File agents/scripts/core/merge-watcher-notify-setup.ps1
#
# Optional:
#   -NoTest        Install only; skip the confirmation toast.
#   -Force         Reinstall BurntToast even if already present.
#
# ASCII-only on purpose: this file is read by Windows PowerShell 5.1, which
# mis-decodes non-ASCII (em-dashes etc.) in a no-BOM UTF-8 file. Keep it ASCII.
# ----------------------------------------------------------------------------

param(
    [switch]$NoTest,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

function Write-Step($msg) { Write-Host "  $msg" }

Write-Host "merge-watcher notify setup -- BurntToast" -ForegroundColor Cyan

$existing = Get-Module -ListAvailable -Name BurntToast | Select-Object -First 1
if ($existing -and -not $Force) {
    Write-Step "[OK] BurntToast already installed (v$($existing.Version))."
} else {
    if ($Force -and $existing) {
        Write-Step "Reinstalling BurntToast (-Force) over v$($existing.Version)..."
    } else {
        Write-Step "BurntToast not found -- installing for the current user..."
    }

    # PSGallery over TLS 1.2 -- older Windows PowerShell defaults to TLS 1.0/1.1
    # which PSGallery rejects, surfacing as an opaque "unable to download".
    try {
        [Net.ServicePointManager]::SecurityProtocol = `
            [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    } catch {
        Write-Step "(note: could not raise TLS to 1.2: $_ -- continuing)"
    }

    # Ensure the NuGet provider exists (Install-Module needs it; first-run
    # machines lack it and would otherwise prompt interactively).
    $nuget = Get-PackageProvider -Name NuGet -ErrorAction SilentlyContinue
    if (-not $nuget -or $nuget.Version -lt [version]"2.8.5.201") {
        Write-Step "Bootstrapping NuGet package provider..."
        Install-PackageProvider -Name NuGet -MinimumVersion 2.8.5.201 `
            -Scope CurrentUser -Force -ForceBootstrap | Out-Null
    }

    # -Force suppresses the "untrusted PSGallery" confirmation prompt without
    # permanently flipping the repository InstallationPolicy to Trusted (a
    # machine-wide change we do not want to make on the user's behalf).
    # -AllowClobber avoids failures if a command name already exists.
    Install-Module -Name BurntToast -Scope CurrentUser -Force -AllowClobber

    $now = Get-Module -ListAvailable -Name BurntToast | Select-Object -First 1
    if (-not $now) {
        Write-Error "BurntToast install reported success but the module is still not discoverable. Check: Get-Module -ListAvailable BurntToast"
        exit 1
    }
    Write-Step "[OK] BurntToast installed (v$($now.Version))."
}

Import-Module BurntToast -ErrorAction Stop

if ($NoTest) {
    Write-Host ""
    Write-Host "[OK] Setup complete (test toast skipped via -NoTest)." -ForegroundColor Green
    exit 0
}

Write-Step "Firing a test toast -- check the bottom-right + Action Center..."
try {
    New-BurntToastNotification `
        -Text @("Smatchet watcher", "Notifications are wired. You will be pinged when a watched PR merges or gets stuck.") `
        -UniqueIdentifier "smatchet-merge-watcher-notify-setup"
} catch {
    Write-Error "Test toast failed even though BurntToast imported: $_"
    exit 1
}

Write-Host ""
Write-Host "[OK] Test toast dispatched." -ForegroundColor Green
Write-Host "  If you saw it, the watcher will now reach you on terminal states"
Write-Host "  (MERGED / stuck / budget-exhausted) via agents/scripts/core/smatchet-notify.sh."
Write-Host "  If you did NOT see it, check Windows Focus Assist / notification settings"
Write-Host "  for the 'Windows PowerShell' app."
