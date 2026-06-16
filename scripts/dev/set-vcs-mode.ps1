<#.
    set-vcs-mode.ps1 - set the Smatchet VCS backend mode (git | p4) on this machine.

    Smatchet's VCS layer is selected by two env vars (AGENTS.md - Dual-VCS topology):
      SMATCHET_AGENT_VCS     git | p4             - ship-loop variant (git ship-line vs P4-gated)
      SMATCHET_LOCK_BACKEND  git-ref | p4-counter - plan-lock backend the lock-*.sh scripts dispatch to

    Both vars must agree, and on Windows BOTH layers must agree: PowerShell inherits
    the Windows User-registry env, while git-bash sources ~/.bashrc which can override
    it. A silent divergence (registry=git, .bashrc=p4) routes lock-claim.sh to the p4
    path and fails "P4USER not set". This script sets BOTH layers idempotently so
    every shell on the machine resolves the same mode. It is the authoritative
    setter on Windows; the .sh sibling does the same job from git-bash / POSIX.

    Open a NEW shell (PowerShell or git-bash) for the change to take effect -
    already-running processes keep their old env.

    Usage:
      pwsh scripts/dev/set-vcs-mode.ps1 git    # git ship-line (default mode)
      pwsh scripts/dev/set-vcs-mode.ps1 p4     # opt into the Perforce local layer
      pwsh scripts/dev/set-vcs-mode.ps1        # print the current mode and exit

    Test hooks (also honour $env:SMATCHET_VCS_RC / $env:SMATCHET_VCS_SKIP_REGISTRY
    for parity with the .sh sibling - not for normal use):
      -RcPath <path>   override the rc file the managed block is written to
      -SkipRegistry    skip the Windows User-registry env update
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('git', 'p4', 'show')]
    [string]$Mode = 'show',

    [string]$RcPath,
    [switch]$SkipRegistry
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# Single source of truth for the value mapping (mirrors set-vcs-mode.sh).
$ModeMap = @{
    git = @{ Vcs = 'git'; Backend = 'git-ref' }
    p4  = @{ Vcs = 'p4';  Backend = 'p4-counter' }
}

$BlockBegin = '# >>> smatchet vcs-mode >>>'
$BlockEnd   = '# <<< smatchet vcs-mode <<<'

function Resolve-RcPath {
    if (-not [string]::IsNullOrWhiteSpace($RcPath))             { return $RcPath }
    if (-not [string]::IsNullOrWhiteSpace($env:SMATCHET_VCS_RC)) { return $env:SMATCHET_VCS_RC }
    return (Join-Path $HOME '.bashrc')
}

function Show-Mode {
    $vcs = if ($env:SMATCHET_AGENT_VCS)    { $env:SMATCHET_AGENT_VCS }    else { '<unset> (default git)' }
    $be  = if ($env:SMATCHET_LOCK_BACKEND) { $env:SMATCHET_LOCK_BACKEND } else { '<unset> (default git-ref)' }
    Write-Host "This shell:    SMATCHET_AGENT_VCS=$vcs  SMATCHET_LOCK_BACKEND=$be"
    $rVcs = [Environment]::GetEnvironmentVariable('SMATCHET_AGENT_VCS', 'User')
    $rBe  = [Environment]::GetEnvironmentVariable('SMATCHET_LOCK_BACKEND', 'User')
    Write-Host "User registry: SMATCHET_AGENT_VCS=$rVcs  SMATCHET_LOCK_BACKEND=$rBe"
}

# Rewrite the managed block in the rc file, stripping any prior managed block +
# legacy unmarked exports first. Idempotent. Writes LF-only / no BOM so git-bash
# never reads a stray CR into the value.
function Set-RcBlock {
    param([string]$Rc, [string]$Vcs, [string]$Backend)

    $existing = @()
    if (Test-Path -LiteralPath $Rc) {
        $existing = @(Get-Content -LiteralPath $Rc)
    } else {
        $dir = Split-Path -Parent $Rc
        if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    }

    $kept = New-Object System.Collections.Generic.List[string]
    $skip = $false
    foreach ($line in $existing) {
        if ($line -eq $BlockBegin) { $skip = $true;  continue }
        if ($line -eq $BlockEnd)   { $skip = $false; continue }
        if ($skip) { continue }
        if ($line -match '^# Smatchet (dual-VCS opt-in|VCS mode)') { continue }
        if ($line -match '^\s*export\s+SMATCHET_AGENT_VCS=')       { continue }
        if ($line -match '^\s*export\s+SMATCHET_LOCK_BACKEND=')    { continue }
        $kept.Add($line)
    }

    $kept.Add($BlockBegin)
    $kept.Add('# Managed by scripts/dev/set-vcs-mode.{sh,ps1} -- do not edit by hand.')
    $kept.Add("export SMATCHET_AGENT_VCS=$Vcs")
    $kept.Add("export SMATCHET_LOCK_BACKEND=$Backend")
    $kept.Add($BlockEnd)

    $text = ($kept -join "`n") + "`n"
    [System.IO.File]::WriteAllText($Rc, $text, (New-Object System.Text.UTF8Encoding($false)))
}

if ($Mode -eq 'show') { Show-Mode; return }

$sel = $ModeMap[$Mode]
$rc  = Resolve-RcPath
Set-RcBlock -Rc $rc -Vcs $sel.Vcs -Backend $sel.Backend

$skipReg = $SkipRegistry.IsPresent -or ($env:SMATCHET_VCS_SKIP_REGISTRY -eq '1')
if (-not $skipReg) {
    [Environment]::SetEnvironmentVariable('SMATCHET_AGENT_VCS',    $sel.Vcs,     'User')
    [Environment]::SetEnvironmentVariable('SMATCHET_LOCK_BACKEND', $sel.Backend, 'User')
}

Write-Host "set-vcs-mode: mode '$Mode' set." -ForegroundColor Green
Write-Host "  rc file: $rc"
Write-Host "    export SMATCHET_AGENT_VCS=$($sel.Vcs)"
Write-Host "    export SMATCHET_LOCK_BACKEND=$($sel.Backend)"
if (-not $skipReg) {
    Write-Host "  Windows User registry: SMATCHET_AGENT_VCS=$($sel.Vcs), SMATCHET_LOCK_BACKEND=$($sel.Backend)"
}
Write-Host "  Open a NEW shell (PowerShell or git-bash) for the change to take effect."
