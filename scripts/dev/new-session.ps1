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

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

# Are we in the main/integration tree? The main worktree has a .git DIRECTORY;
# a linked worktree has a .git FILE (gitdir pointer).
$gitPath = Join-Path $RepoRoot '.git'
$isIntegrationTree = (Test-Path -LiteralPath $gitPath -PathType Container)

if (-not (Get-Command claude -ErrorAction SilentlyContinue)) {
    throw "claude CLI not found on PATH."
}

if ($isIntegrationTree) {
    if ([string]::IsNullOrWhiteSpace($Slug)) {
        throw "You are in the integration tree ($RepoRoot). Pass a slug to spin an isolated worktree: new-session.ps1 <slug>"
    }
    $worktreeScript = Join-Path $PSScriptRoot 'worktree.ps1'
    & $worktreeScript new $Slug
    if ($LASTEXITCODE -ne 0) { throw "worktree.ps1 new $Slug failed" }

    $treesRoot = if ([string]::IsNullOrWhiteSpace($env:SMATCHET_TREES_ROOT)) { 'C:\Dev\trees' } else { $env:SMATCHET_TREES_ROOT }
    $target = Join-Path $treesRoot $Slug
    Write-Host "Launching claude in $target ..." -ForegroundColor Green
    Push-Location $target
    try { & claude } finally { Pop-Location }
} else {
    Write-Host "Already in an isolated worktree ($RepoRoot) - launching claude here." -ForegroundColor Green
    & claude
}
