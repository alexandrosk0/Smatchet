# Set up a per-harness adapter directory by linking into the canonical
# `agents/` tree and copying small adapter templates from
# `docs/harness/<name>/`. Idempotent.
#
# Supported harnesses: claude-code | codex | cursor
#
# Why links: keeps `agents/*.md` the single source of truth — edits to canonical
# agent definitions are visible to the harness immediately, no sync step.
# Templates (`settings.json`, hook shell scripts) are copies so per-machine
# tweaks don't propagate back to the tracked template.
#
# Windows: directory junctions (`mklink /J`) and file hardlinks (`mklink /H`)
# work without admin / Dev Mode. If Dev Mode is on, real file symlinks are
# preferred and tried first.

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Harness
)

$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
$Root = (Get-Location).Path

if (-not $Harness -or $Harness -in @('-h', '--help', '/?')) {
    Write-Host @'
Usage: pwsh scripts/setup-harness.ps1 <harness>

Harnesses:
  claude-code   Generate .claude/ with junctions/symlinks into agents/ +
                copies of settings.json, CLAUDE.md, lint hooks.
  codex         No local setup needed — Codex reads AGENTS.md directly per
                the agents.md spec. Run for confirmation.
  cursor        Generate .cursor/rules/agents.mdc.

Idempotent — re-running is safe. Edits to .claude/settings.json (or other
templates) survive: the script skips copies if the local file differs from
the tracked template.

After clone, run the script for the harness you use. See
docs/harness/SETUP.md for the full mapping.
'@
    return
}

function Link-Dir {
    param([string]$Link, [string]$TargetRel)
    if ((Test-Path $Link)) { return }
    $linkDir = Split-Path -Parent $Link
    if ($linkDir -and -not (Test-Path $linkDir)) { New-Item -ItemType Directory -Path $linkDir | Out-Null }
    $TargetAbs = Join-Path $Root $TargetRel
    & cmd /c mklink /J "$Link" "$TargetAbs" | Out-Null
    Write-Host "  link-dir   $Link -> $TargetRel"
}

function Link-File {
    param([string]$Link, [string]$TargetRel)
    if (Test-Path $Link) { return }
    $linkDir = Split-Path -Parent $Link
    if ($linkDir -and -not (Test-Path $linkDir)) { New-Item -ItemType Directory -Path $linkDir | Out-Null }
    $TargetAbs = Join-Path $Root $TargetRel
    # Try real symlink (needs Dev Mode or admin); fall back to hardlink.
    $sym = & cmd /c mklink "$Link" "$TargetAbs" 2>$null
    if ($LASTEXITCODE -ne 0) {
        & cmd /c mklink /H "$Link" "$TargetAbs" | Out-Null
    }
    Write-Host "  link-file  $Link -> $TargetRel"
}

function Copy-Template {
    param([string]$Src, [string]$Dst)
    $dstDir = Split-Path -Parent $Dst
    if ($dstDir -and -not (Test-Path $dstDir)) { New-Item -ItemType Directory -Path $dstDir | Out-Null }
    if (Test-Path $Dst) {
        $a = Get-FileHash -Algorithm SHA256 $Src
        $b = Get-FileHash -Algorithm SHA256 $Dst
        if ($a.Hash -eq $b.Hash) { return }
        Write-Host "  skip-copy  $Dst (user-modified — not overwriting)"
        return
    }
    Copy-Item -LiteralPath $Src -Destination $Dst
    Write-Host "  copy       $Dst"
}

function Setup-ClaudeCode {
    Write-Host 'Setting up Claude Code adapter at .claude\ ...'

    Copy-Template 'docs\harness\claude-code\CLAUDE.md.tmpl'           '.claude\CLAUDE.md'
    Copy-Template 'docs\harness\claude-code\settings.json.tmpl'       '.claude\settings.json'
    Copy-Template 'docs\harness\claude-code\hooks\lint-cpp.sh'        '.claude\hooks\lint-cpp.sh'
    Copy-Template 'docs\harness\claude-code\hooks\vexp-guard.sh'      '.claude\hooks\vexp-guard.sh'
    Copy-Template 'docs\harness\claude-code\hooks\lint-syntax-both.py' '.claude\hooks\lint-syntax-both.py'

    Link-Dir  '.claude\agents'                       'agents'
    Link-Dir  '.claude\skills\grill-with-docs'       'agents\_shared\skills\grill-with-docs'
    Link-Dir  '.claude\skills\scratchpad-recall'     'agents\_shared\skills\scratchpad-recall'
    Link-File '.claude\skills\agent-tokens\SKILL.md' 'agents\_shared\token-tracking\SKILL.md'
    Link-File '.claude\hooks\agent-token-log.py'     'agents\_shared\token-tracking\agent-token-log.py'
    Link-File '.claude\hooks\agents-statusline.py'   'agents\_shared\token-tracking\agents-statusline.py'

    Write-Host 'Done. .claude\ ready for Claude Code.'
}

function Setup-Codex {
    Write-Host 'Codex / OpenAI Agents reads AGENTS.md + agents\*.md directly per the'
    Write-Host 'agents.md spec — no local adapter required.'
    Write-Host ''
    Write-Host 'Verify:'
    if (Test-Path 'AGENTS.md') { Write-Host '  OK  AGENTS.md present at repo root' }
    else { Write-Host '  FAIL AGENTS.md missing'; exit 1 }
    $count = (Get-ChildItem agents -Filter '*.md' -File | Where-Object { $_.Name -ne 'README.md' }).Count
    Write-Host "  OK  agents\*.md = $count files"
}

function Setup-Cursor {
    Write-Host 'Setting up Cursor adapter at .cursor\ ...'
    # Cursor accepts .cursor\rules as either a file or a dir of .mdc rules.
    # Some tools (e.g. vexp) write to .cursor\rules as a single file. Bail
    # with a fix-it message rather than clobbering user state.
    if (Test-Path '.cursor\rules' -PathType Leaf) {
        Write-Error '.cursor\rules exists as a file (not a directory). Move it aside: `Move-Item .cursor\rules .cursor\rules.bak`. Then re-run.'
        exit 1
    }
    Copy-Template 'docs\harness\cursor\rules\agents.mdc' '.cursor\rules\agents.mdc'
    Write-Host 'Done. .cursor\rules\agents.mdc points Cursor at AGENTS.md + agents\.'
}

switch ($Harness) {
    'claude-code' { Setup-ClaudeCode }
    'codex'       { Setup-Codex }
    'cursor'      { Setup-Cursor }
    default {
        Write-Error "unknown harness '$Harness'. Supported: claude-code | codex | cursor"
        exit 1
    }
}
