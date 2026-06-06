# Set up a per-harness adapter directory by linking into the canonical
# `agents/` tree and copying small adapter templates from
# `docs/harness/<name>/`. Idempotent.
#
# Supported harnesses: claude-code | codex | cursor | pi
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
Set-Location (Join-Path $PSScriptRoot '..\..\..')
$Root = (Get-Location).Path

if (-not $Harness -or $Harness -in @('-h', '--help', '/?')) {
    Write-Host @'
Usage: pwsh agents/scripts/core/setup-harness.ps1 <harness>

Harnesses:
  claude-code   Generate .claude/ with junctions/symlinks into agents/ +
                copies of settings.json, CLAUDE.md, lint hooks.
  codex         No local setup needed — Codex reads AGENTS.md directly per
                the agents.md spec. Run for confirmation.
  cursor        Generate .cursor/rules/agents.mdc.
  pi            Generate .pi\agents\*.md (pi-native) + patched subagent extension.

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

function Gen-SubsystemClaudeShims {
    # Gitignored CLAUDE.md shims beside each subsystem leaf AGENTS.md so Claude
    # Code lazy-loads the leaf rules (it reads nested CLAUDE.md, not nested
    # AGENTS.md). Committed tree stays AGENTS.md-only. Registry: root CONTEXT-MAP.md.
    $count = 0
    foreach ($agents in (git ls-files 'Source/Core/src/*/AGENTS.md')) {
        if (-not $agents) { continue }
        $shim = Join-Path (Split-Path -Parent $agents) 'CLAUDE.md'
        $needs = $true
        if ((Test-Path $shim) -and (Select-String -Path $shim -Pattern '^@AGENTS.md' -Quiet)) { $needs = $false }
        if ($needs) {
            "@AGENTS.md`n`n<!-- Generated gitignored shim (setup-harness.ps1). Claude Code lazy-loads nested CLAUDE.md, not AGENTS.md; this imports the sibling leaf rules. Edit AGENTS.md, not this file. -->" | Set-Content -Path $shim -Encoding utf8
            $count++
        }
    }
    Write-Host "  subsystem-shims  $count new CLAUDE.md shim(s) beside Source/Core/src/*/AGENTS.md (gitignored)"
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

    Gen-SubsystemClaudeShims

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

function Setup-Pi {
    Write-Host 'Setting up pi adapter at .pi\ ...'
    # 1. Generate the pi-native agent files (flatten + capability->tools + model map).
    $py = (Get-Command python3 -ErrorAction SilentlyContinue) ?? (Get-Command python -ErrorAction SilentlyContinue)
    if (-not $py) { Write-Error 'python3 not found - needed to generate .pi\agents\*.md'; exit 1 }
    & $py.Source 'agents/scripts/core/gen-pi-agents.py' (Get-Location).Path (Join-Path (Get-Location).Path '.pi/agents')

    # 2. Install + patch the subagent extension from the installed pi package.
    $dst = '.pi\extensions\subagent'
    $cands = @($env:PI_PACKAGE_DIR,
               (Join-Path (& npm root -g 2>$null) '@earendil-works/pi-coding-agent'),
               (Join-Path $env:APPDATA 'npm\node_modules\@earendil-works\pi-coding-agent'))
    $pkg = $cands | Where-Object { $_ -and (Test-Path (Join-Path $_ 'examples/extensions/subagent')) } | Select-Object -First 1
    if ($pkg) {
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        Copy-Template (Join-Path $pkg 'examples\extensions\subagent\index.ts')  (Join-Path $dst 'index.ts')
        Copy-Template (Join-Path $pkg 'examples\extensions\subagent\agents.ts') (Join-Path $dst 'agents.ts')
        $idx = Join-Path $dst 'index.ts'
        $txt = Get-Content -Raw $idx
        if ($txt -match 'params\.agentScope \?\? "user"') {
            $txt = $txt -replace 'params\.agentScope \?\? "user"', 'params.agentScope ?? "both"'
            $txt = $txt -replace 'params\.confirmProjectAgents \?\? true', 'params.confirmProjectAgents ?? false'
            $txt = $txt -replace 'args\.agentScope \?\? "user"', 'args.agentScope ?? "both"'
            Set-Content -NoNewline -Path $idx -Value $txt
            Write-Host "  patch      $idx (agentScope=both, confirmProjectAgents=false)"
        } else {
            Write-Host "  patch      $idx already patched (skip)"
        }
    } else {
        Write-Host '  WARN  pi package not found - set PI_PACKAGE_DIR and re-run. Agents were still generated.'
    }
    Write-Host 'Done. .pi\ ready for pi. See docs/harness/pi/README.md.'
}

switch ($Harness) {
    'claude-code' { Setup-ClaudeCode }
    'codex'       { Setup-Codex }
    'cursor'      { Setup-Cursor }
    'pi'          { Setup-Pi }
    default {
        Write-Error "unknown harness '$Harness'. Supported: claude-code | codex | cursor | pi"
        exit 1
    }
}
