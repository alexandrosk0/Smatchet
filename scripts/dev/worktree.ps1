<#.
    worktree.ps1 - isolated-session worktree lifecycle for concurrent Claude Code sessions.

    Each concurrent session should live in its OWN git worktree (own HEAD / index /
    working tree) so a branch switch, pull, or reset in one session cannot corrupt
    another. The main clone (C:\Dev\Smatchet) is the stable INTEGRATION tree -
    pinned to develop, used for pull/merge/review, never for feature work.

    See docs/agent-rules/process-rules.md - Concurrent interactive sessions.

    Subcommands:
      new <slug>     Create a worktree at <TreesRoot>\<slug> on feat/<slug> off
                     origin/develop, wire its .claude/ adapter, print the launch line.
      resync         Re-baseline every session entry rooted in THIS tree to current
                     HEAD - the recovery path after a HEAD-drift block (accept
                     "I'm on this branch now" without switching the shared HEAD).
      list           Show all worktrees + branch / dirty / live-session count.
      rm <slug>      Remove the worktree (and optionally its branch).
      prune          git worktree prune + report worktrees already merged to develop.

    TreesRoot defaults to C:\Dev\trees (override: $env:SMATCHET_TREES_ROOT).

    Examples:
      pwsh scripts/dev/worktree.ps1 new vsync-toggle
      pwsh scripts/dev/worktree.ps1 list
      pwsh scripts/dev/worktree.ps1 rm vsync-toggle -DeleteBranch
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('new', 'resync', 'list', 'rm', 'prune')]
    [string]$Command,

    [Parameter(Position = 1)]
    [string]$Slug,

    [string]$Base = 'develop',
    [string]$Branch,
    [switch]$Force,
    [switch]$DeleteBranch
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
# PS 7+ honors this so native stderr (git progress) doesn't terminate under Stop.
$PSNativeCommandUseErrorActionPreference = $false

# scripts/dev/worktree.ps1 -> repo root is two levels up. When run from a worktree
# this is that worktree (its .git points at the shared object store, so worktree
# add still registers globally).
$RepoRoot  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$TreesRoot = if ([string]::IsNullOrWhiteSpace($env:SMATCHET_TREES_ROOT)) { 'C:\Dev\trees' } else { $env:SMATCHET_TREES_ROOT }

function Invoke-Native {
    <# Run a native command, tolerating its stderr under Stop; validate $LASTEXITCODE. #>
    param(
        [Parameter(Mandatory = $true)] [string]$FailureMessage,
        [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)] [string[]]$Arguments
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Arguments[0] @($Arguments | Select-Object -Skip 1) }
    finally { $ErrorActionPreference = $previous }
    if ($LASTEXITCODE -ne 0) { throw "$FailureMessage (exit code $LASTEXITCODE)" }
}

function Get-GitOut {
    <# Capture git stdout (trimmed); '' on failure. Pass args as an explicit array
       (@('-C', $path, ...)) so a leading -C is data, not a misbound parameter. #>
    param([Parameter(Mandatory = $true)] [string[]]$GitArgs)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { $out = & git @GitArgs 2>$null } finally { $ErrorActionPreference = $previous }
    if ($LASTEXITCODE -ne 0) { return '' }
    return ($out | Out-String).Trim()
}

function Test-GitRef {
    param([string]$Ref)
    & git -C $RepoRoot rev-parse --verify --quiet $Ref *> $null
    return ($LASTEXITCODE -eq 0)
}

function Get-BashExe {
    <# Resolve git-bash specifically (PATH 'bash' may be WSL, which can't read
       Windows C:\ paths). Fall back to 'bash' if git-bash isn't found. #>
    $cands = New-Object System.Collections.Generic.List[string]
    $g = Get-Command git -ErrorAction SilentlyContinue
    if ($g) {
        $gitRoot = Split-Path -Parent (Split-Path -Parent $g.Source)
        $cands.Add((Join-Path $gitRoot 'bin\bash.exe'))
        $cands.Add((Join-Path $gitRoot 'usr\bin\bash.exe'))
    }
    $cands.Add('C:\Program Files\Git\bin\bash.exe')
    $cands.Add('C:\Program Files\Git\usr\bin\bash.exe')
    foreach ($c in $cands) { if (Test-Path -LiteralPath $c) { return $c } }
    return 'bash'
}

function Assert-SafeSlug {
    <# Guard against path traversal / shell-odd slugs before they reach paths +
       branch names: letters, digits, dot, underscore, hyphen only. #>
    param([string]$Value)
    if ($Value -eq '.' -or $Value -eq '..' -or $Value -notmatch '^[A-Za-z0-9._-]+$') {
        throw "Invalid slug '$Value' - use only letters, digits, dot, underscore, hyphen (no spaces or path separators)."
    }
}

# --- Per-tree session registry (mirrors the bash hooks' format) --------------
function Get-RegistryDir { param([string]$Tree) Join-Path (Join-Path $Tree '.claude') '.active-sessions' }

function Get-LiveSessionCount {
    param([string]$Tree)
    $dir = Get-RegistryDir $Tree
    if (-not (Test-Path -LiteralPath $dir)) { return 0 }
    $now = [int][double]::Parse((Get-Date -UFormat %s))
    $live = 0
    foreach ($f in (Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue)) {
        $ts = 0; $procId = 0
        foreach ($line in (Get-Content -LiteralPath $f.FullName -ErrorAction SilentlyContinue)) {
            if ($line -match '^ts=(\d+)')   { $ts     = [int]$Matches[1] }
            if ($line -match '^ppid=(\d+)') { $procId = [int]$Matches[1] }
        }
        $fresh = ($ts -gt 0) -and (($now - $ts) -lt 1800)
        $alive = $false
        if ($procId -gt 0) { $alive = $null -ne (Get-Process -Id $procId -ErrorAction SilentlyContinue) }
        if ($fresh -or $alive) { $live++ }
    }
    return $live
}

function Invoke-New {
    if ([string]::IsNullOrWhiteSpace($Slug)) { throw "new requires a <slug>: worktree.ps1 new <slug>" }
    Assert-SafeSlug $Slug
    $branchName = if ([string]::IsNullOrWhiteSpace($Branch)) { "feat/$Slug" } else { $Branch }
    $path = Join-Path $TreesRoot $Slug
    if (Test-Path -LiteralPath $path) { throw "Path already exists: $path" }
    if (-not (Test-Path -LiteralPath $TreesRoot)) { New-Item -ItemType Directory -Path $TreesRoot -Force | Out-Null }

    # First-run bootstrap: close the #913 fresh-clone hole. The HEAD-drift guard
    # lives in the gitignored .claude/, wired only by setup-harness. If THIS tree
    # (the integration clone the launcher runs from) has never been provisioned,
    # its guard is INACTIVE - so a concurrent session in the main clone is
    # unprotected until someone remembers to run setup-harness. Provision it now,
    # before spinning the worktree, so the standard launcher closes the hole
    # without the user remembering. Idempotent (skips once the guard hook exists);
    # non-fatal (a failure just warns - worktree creation still proceeds).
    # See docs/harness/SETUP.md - Concurrent-session HEAD-drift guard.
    $repoGuard = Join-Path $RepoRoot '.claude\hooks\guard-head-drift.sh'
    if (-not (Test-Path -LiteralPath $repoGuard)) {
        $bashBoot  = Get-BashExe
        $setupBoot = (Join-Path $RepoRoot 'agents/scripts/core/setup-harness.sh') -replace '\\', '/'
        Write-Host "First run: provisioning .claude/ in the integration tree ($RepoRoot) so its HEAD-drift guard is active ..." -ForegroundColor DarkGray
        try { Invoke-Native -FailureMessage "setup-harness (integration tree) failed" -Arguments $bashBoot, $setupBoot, "claude-code" }
        catch { Write-Host "  WARNING: could not provision the integration tree's .claude/ - run 'bash agents/scripts/core/setup-harness.sh claude-code' there manually. ($_)" -ForegroundColor Yellow }
    }

    Write-Host "Fetching origin/$Base ..." -ForegroundColor DarkGray
    try { Invoke-Native -FailureMessage "fetch failed" -Arguments "git", "-C", $RepoRoot, "fetch", "--quiet", "origin", $Base }
    catch { Write-Host "  (fetch failed - using local $Base)" -ForegroundColor Yellow }

    $baseRef = if (Test-GitRef "origin/$Base") { "origin/$Base" } else { $Base }

    if (Test-GitRef "refs/heads/$branchName") {
        Write-Host "Branch $branchName exists - attaching worktree." -ForegroundColor DarkGray
        Invoke-Native -FailureMessage "worktree add failed" -Arguments "git", "-C", $RepoRoot, "worktree", "add", $path, $branchName
    } else {
        Invoke-Native -FailureMessage "worktree add failed" -Arguments "git", "-C", $RepoRoot, "worktree", "add", "-b", $branchName, $path, $baseRef
    }

    # Wire the worktree's OWN .claude/ adapter (hooks incl. the drift guard) by
    # running the worktree's copy of setup-harness (it resolves ROOT from its own
    # location, so this provisions $path, not the main clone). Use git-bash
    # explicitly - the PATH 'bash' may be WSL, which can't read C:\ paths.
    $bash  = Get-BashExe
    $setup = (Join-Path $path 'agents/scripts/core/setup-harness.sh') -replace '\\', '/'
    Write-Host "Wiring .claude/ adapter in the new worktree (via $bash) ..." -ForegroundColor DarkGray
    $wired = $true
    try { Invoke-Native -FailureMessage "setup-harness failed" -Arguments $bash, $setup, "claude-code" }
    catch { $wired = $false }

    $settings = Join-Path $path '.claude\settings.json'
    Write-Host ""
    if ($wired -and (Test-Path -LiteralPath $settings)) {
        Write-Host "Worktree ready: $path  (branch $branchName)" -ForegroundColor Green
        Write-Host "Launch a session there:" -ForegroundColor Green
        Write-Host "  cd `"$path`"; claude"
    } else {
        Write-Host "WARNING: worktree created at $path (branch $branchName) but .claude/ is NOT wired -" -ForegroundColor Yellow
        Write-Host "         the HEAD-drift guard is INACTIVE there. Finish setup before relying on isolation:" -ForegroundColor Yellow
        Write-Host "         `"$bash`" `"$setup`" claude-code" -ForegroundColor Yellow
    }
}

function Invoke-Resync {
    $tree = (Get-GitOut @('-C', $RepoRoot, 'rev-parse', '--show-toplevel'))
    if ([string]::IsNullOrWhiteSpace($tree)) { $tree = $RepoRoot }
    $curBranch = (Get-GitOut @('-C', $tree, 'symbolic-ref', '--short', 'HEAD'))
    $curSha    = (Get-GitOut @('-C', $tree, 'rev-parse', 'HEAD'))
    if ([string]::IsNullOrWhiteSpace($curBranch) -or [string]::IsNullOrWhiteSpace($curSha)) {
        throw "Cannot resolve current HEAD (detached or mid-rebase?) in $tree"
    }
    $dir = Get-RegistryDir $tree
    if (-not (Test-Path -LiteralPath $dir)) { Write-Host "No session registry in this tree - nothing to resync." -ForegroundColor Yellow; return }
    $now = [int][double]::Parse((Get-Date -UFormat %s))
    $count = 0
    foreach ($f in (Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue)) {
        $procId = 0
        foreach ($line in (Get-Content -LiteralPath $f.FullName -ErrorAction SilentlyContinue)) {
            if ($line -match '^ppid=(\d+)') { $procId = [int]$Matches[1] }
        }
        $body = "branch=$curBranch`nsha=$curSha`nppid=$procId`nts=$now`n"
        Set-Content -LiteralPath $f.FullName -Value $body -NoNewline -Encoding ASCII
        $count++
    }
    Write-Host "Re-baselined $count session entry(ies) in $tree to $curBranch@$($curSha.Substring(0,8))." -ForegroundColor Green
}

function Invoke-List {
    $porcelain = (Get-GitOut @('-C', $RepoRoot, 'worktree', 'list', '--porcelain'))
    if ([string]::IsNullOrWhiteSpace($porcelain)) { Write-Host "No worktrees."; return }
    $wt = $null
    Write-Host ("{0,-44} {1,-26} {2,-6} {3}" -f "PATH", "BRANCH", "DIRTY", "LIVE") -ForegroundColor Cyan
    foreach ($line in ($porcelain -split "`n")) {
        $line = $line.TrimEnd("`r")
        if ($line -match '^worktree (.+)$') { $wt = $Matches[1] }
        elseif ($line -match '^branch (.+)$' -and $wt) {
            $br = $Matches[1] -replace '^refs/heads/', ''
            $dirty = if ([string]::IsNullOrWhiteSpace((Get-GitOut @('-C', $wt, 'status', '--short')))) { "clean" } else { "DIRTY" }
            $live = Get-LiveSessionCount $wt
            Write-Host ("{0,-44} {1,-26} {2,-6} {3}" -f $wt, $br, $dirty, $live)
            $wt = $null
        }
        elseif ($line -match '^detached' -and $wt) {
            $live = Get-LiveSessionCount $wt
            Write-Host ("{0,-44} {1,-26} {2,-6} {3}" -f $wt, "(detached)", "-", $live)
            $wt = $null
        }
    }
}

function Invoke-Rm {
    if ([string]::IsNullOrWhiteSpace($Slug)) { throw "rm requires a <slug>: worktree.ps1 rm <slug>" }
    if (-not (Test-Path -LiteralPath $Slug)) { Assert-SafeSlug $Slug }
    $path = if (Test-Path -LiteralPath $Slug) { $Slug } else { Join-Path $TreesRoot $Slug }
    $rmArgs = @("git", "-C", $RepoRoot, "worktree", "remove", $path)
    if ($Force) { $rmArgs += "--force" }
    Invoke-Native -FailureMessage "worktree remove failed" -Arguments $rmArgs
    Write-Host "Removed worktree: $path" -ForegroundColor Green
    if ($DeleteBranch) {
        $branchName = if ([string]::IsNullOrWhiteSpace($Branch)) { "feat/$Slug" } else { $Branch }
        $flag = if ($Force) { "-D" } else { "-d" }
        try { Invoke-Native -FailureMessage "branch delete failed" -Arguments "git", "-C", $RepoRoot, "branch", $flag, $branchName; Write-Host "Deleted branch: $branchName" -ForegroundColor Green }
        catch { Write-Host "  branch $branchName not deleted (unmerged? use -Force): $_" -ForegroundColor Yellow }
    }
}

function Invoke-Prune {
    Invoke-Native -FailureMessage "worktree prune failed" -Arguments "git", "-C", $RepoRoot, "worktree", "prune"
    Write-Host "Pruned stale worktree admin entries." -ForegroundColor Green
    $porcelain = (Get-GitOut @('-C', $RepoRoot, 'worktree', 'list', '--porcelain'))
    $wt = $null
    foreach ($line in ($porcelain -split "`n")) {
        $line = $line.TrimEnd("`r")
        if ($line -match '^worktree (.+)$') { $wt = $Matches[1] }
        elseif ($line -match '^branch (.+)$' -and $wt) {
            $br = $Matches[1] -replace '^refs/heads/', ''
            if ($br -ne 'develop' -and $br -ne 'main') {
                & git -C $RepoRoot merge-base --is-ancestor $br origin/develop *> $null
                if ($LASTEXITCODE -eq 0) { Write-Host "  MERGED -> safe to remove: $wt ($br)" -ForegroundColor DarkGray }
            }
            $wt = $null
        }
        elseif ($line -match '^detached') { $wt = $null }
    }
}

switch ($Command) {
    'new'    { Invoke-New }
    'resync' { Invoke-Resync }
    'list'   { Invoke-List }
    'rm'     { Invoke-Rm }
    'prune'  { Invoke-Prune }
}
