<#.
    new-session.ps1 - launch a Claude Code session in an ISOLATED worktree.

    Makes worktree-per-session the default action instead of a discipline you have
    to remember. If you are currently in the shared integration tree it spins up a
    fresh worktree (via worktree.ps1 new <slug>) and launches claude there; if you
    are already inside a worktree it just launches claude in place.

    Recommended PowerShell-profile alias (add to $PROFILE):
      function nsc { param([string]$slug) & "C:\Dev\Smatchet\scripts\dev\new-session.ps1" $slug }

    Then:  nsc vsync-toggle

    See docs/agent-rules/process-rules.md - Concurrent interactive sessions.

    Examples:
      pwsh scripts/dev/new-session.ps1 vsync-toggle
      pwsh scripts/dev/new-session.ps1            # already in a worktree -> just launch
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Slug
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

if (-not (Get-Command claude -ErrorAction SilentlyContinue)) {
    throw "claude CLI not found on PATH."
}

# Detect the tree the CALLER is in (current directory), NOT where this script
# lives - the nsc alias points at one fixed script copy, so script-location would
# always look like the integration tree. The main worktree's toplevel has a .git
# DIRECTORY; a linked worktree's .git is a FILE (gitdir pointer).
$cwd = (Get-Location).Path
$tree = (& git -C $cwd rev-parse --show-toplevel 2>$null)
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($tree)) { $tree = $cwd }
$isIntegrationTree = (Test-Path -LiteralPath (Join-Path $tree '.git') -PathType Container)

if ($isIntegrationTree) {
    if ([string]::IsNullOrWhiteSpace($Slug)) {
        throw "You are in the integration tree ($tree). Pass a slug to spin an isolated worktree: new-session.ps1 <slug>"
    }
    # worktree.ps1 validates the slug + throws on failure (ErrorActionPreference
    # propagates the throw here). $LASTEXITCODE is not a reliable success signal
    # for a .ps1 call, so verify the worktree actually materialised instead.
    $worktreeScript = Join-Path $PSScriptRoot 'worktree.ps1'
    try { & $worktreeScript new $Slug } catch { throw "worktree.ps1 new $Slug failed: $_" }

    $treesRoot = if ([string]::IsNullOrWhiteSpace($env:SMATCHET_TREES_ROOT)) { 'C:\Dev\trees' } else { $env:SMATCHET_TREES_ROOT }
    $target = Join-Path $treesRoot $Slug
    if (-not (Test-Path -LiteralPath $target -PathType Container)) {
        throw "Expected worktree at $target was not created - aborting launch."
    }
    Write-Host "Launching claude in $target ..." -ForegroundColor Green
    Push-Location $target
    try { & claude } finally { Pop-Location }
} else {
    Write-Host "Already in an isolated worktree ($tree) - launching claude here." -ForegroundColor Green
    & claude
}
