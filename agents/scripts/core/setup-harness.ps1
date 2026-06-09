# Set up a per-harness adapter directory by linking into the canonical
# `agents/` tree and copying small adapter templates from
# `docs/harness/<name>/`. Idempotent.
#
# Supported harnesses: claude-code | codex | cursor | pi
#
# Why links: keeps `agents/*.md` the single source of truth - edits to canonical
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
  codex         Generate .codex\ config/hooks/custom agents from tracked
                templates + verify AGENTS.md and repo-owned git hooks.
  cursor        Generate .cursor/rules/agents.mdc.
  pi            Generate .pi\agents\*.md (pi-native) + patched subagent extension.

Idempotent - re-running is safe. Edits to .claude/settings.json (or other
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
        Write-Host "  skip-copy  $Dst (user-modified - not overwriting)"
        return
    }
    Copy-Item -LiteralPath $Src -Destination $Dst
    Write-Host "  copy       $Dst"
}

function Resolve-Python {
    foreach ($name in @('python', 'py', 'python3')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        if ($cmd.Source -like '*\WindowsApps\python*.exe') { continue }
        & $cmd.Source -c "pass" 1>$null 2>$null
        if ($LASTEXITCODE -eq 0) { return $cmd.Source }
    }
    return $null
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

function Install-GitHooks {
    $target = 'scripts/git-hooks'
    $current = (& git config --local --get core.hooksPath 2>$null)
    if ($LASTEXITCODE -ne 0) { $current = '' }
    $current = [string]$current
    $normalizedCurrent = $current.Replace('\', '/')
    $isDefaultHooksPath = ($normalizedCurrent -eq '.git/hooks') -or $normalizedCurrent.EndsWith('/.git/hooks')

    if ([string]::IsNullOrWhiteSpace($current) -or $isDefaultHooksPath) {
        & git config --local core.hooksPath $target
        Write-Host "  git-hooks  core.hooksPath set to $target"
    } elseif ($current -eq $target) {
        Write-Host "  git-hooks  core.hooksPath already $target"
    } else {
        Write-Host "  git-hooks  WARNING: core.hooksPath is '$current' (not '$target'). Skipping."
        Write-Host "             To opt in, run: git config --local core.hooksPath $target"
    }
}

function Setup-Codex {
    Write-Host 'Setting up Codex / OpenAI Agents repo-owned wiring ...'
    Write-Host 'Codex reads AGENTS.md natively; setup adds gitignored .codex\'
    Write-Host 'config/hooks/custom-agent files generated from tracked templates.'
    Write-Host ''
    Copy-Template 'docs\harness\codex\config.toml.tmpl' '.codex\config.toml'
    Copy-Template 'docs\harness\codex\hooks.json.tmpl'  '.codex\hooks.json'

    $py = Resolve-Python
    if (-not $py) {
        Write-Host '  FAIL python not found - needed to generate .codex\agents\*.toml'
        exit 1
    }
    & $py 'agents/scripts/core/gen-codex-agents.py' (Get-Location).Path (Join-Path (Get-Location).Path '.codex/agents')

    Write-Host 'Verify:'
    if (Test-Path 'AGENTS.md') { Write-Host '  OK  AGENTS.md present at repo root' }
    else { Write-Host '  FAIL AGENTS.md missing'; exit 1 }

    $coreAgents = @(Get-ChildItem 'agents\core' -Filter '*.md' -File -ErrorAction SilentlyContinue)
    $projectAgents = @(Get-ChildItem 'agents\project' -Filter '*.md' -File -ErrorAction SilentlyContinue)
    $count = $coreAgents.Count + $projectAgents.Count
    if ($count -eq 0) {
        Write-Host '  FAIL agents\{core,project}\*.md has no agent definitions'
        exit 1
    }
    Write-Host "  OK  agents\{core,project}\*.md = $count files ($($coreAgents.Count) core, $($projectAgents.Count) project)"

    $codexAgents = @(Get-ChildItem '.codex\agents' -Filter '*.toml' -File -ErrorAction SilentlyContinue)
    if ($codexAgents.Count -eq $count) {
        Write-Host "  OK  .codex\agents\*.toml = $($codexAgents.Count) generated custom agent(s)"
    } else {
        Write-Host "  FAIL .codex\agents\*.toml = $($codexAgents.Count) generated custom agent(s), expected $count"
        exit 1
    }

    $dups = @($coreAgents + $projectAgents | Group-Object Name | Where-Object { $_.Count -gt 1 })
    if ($dups.Count -gt 0) {
        Write-Host '  WARN duplicate agent basenames across core/project:'
        foreach ($dup in $dups) { Write-Host "        $($dup.Name)" }
    } else {
        Write-Host '  OK  no duplicate agent basenames across core/project'
    }

    $leafRules = @(& git ls-files 'Source/Core/src/*/AGENTS.md' 2>$null)
    Write-Host "  OK  Codex native nearest-AGENTS leaf rules = $($leafRules.Count) file(s)"

    if ((Test-Path '.codex\config.toml') -and (Test-Path '.codex\hooks.json')) {
        Write-Host '  OK  Codex-native hooks/config installed under .codex\ (trust locally before use)'
    } else {
        Write-Host '  FAIL .codex\config.toml or .codex\hooks.json missing'
        exit 1
    }

    Install-GitHooks

    Write-Host ''
    Write-Host 'Codex parity report:'
    Write-Host '  OK   Agent/rule discovery is native: AGENTS.md + agents\{core,project}\*.md.'
    Write-Host '  OK   Project specialist agents are generated as .codex\agents\*.toml.'
    Write-Host '  OK   Safe SessionStart/Stop command hooks are installed in .codex\hooks.json.'
    Write-Host '  OK   Stable repo hooks are wired through scripts/git-hooks when core.hooksPath allows it.'
    Write-Host '  NOTE Payload-dependent Claude Code hooks remain intentionally unwired in Codex:'
    Write-Host '       PreToolUse edit/HEAD guards, PostToolUse edit lint, Bash PR'
    Write-Host '       autoregistration, and SubagentStop token telemetry need Codex payload'
    Write-Host '       validation before they can block safely. See'
    Write-Host '       docs/harness/codex/hooks-equivalent.md.'
}

function Setup-Cursor {
    Write-Host 'Setting up Cursor adapter at .cursor\ ...'
    # Cursor accepts .cursor\rules as either a file or a dir of .mdc rules.
    # Some tools write to .cursor\rules as a single file. Bail
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
    $py = Resolve-Python
    if (-not $py) { Write-Error 'python3 not found - needed to generate .pi\agents\*.md'; exit 1 }
    & $py 'agents/scripts/core/gen-pi-agents.py' (Get-Location).Path (Join-Path (Get-Location).Path '.pi/agents')

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

function Invoke-RequiredToolsProbe {
    $bash = Get-Command bash -ErrorAction SilentlyContinue
    if (-not $bash) {
        Write-Host 'WARN  bash not found; skipping scripts/dev/check-required-tools.sh probe.'
        Write-Host '      Install Git for Windows or WSL bash, then re-run setup-harness.'
        Write-Host ''
        return
    }

    & $bash.Source 'scripts/dev/check-required-tools.sh'
    if ($LASTEXITCODE -ne 0) {
        Write-Host ''
        Write-Host 'note: continuing setup despite missing required tool(s). Install the flagged'
        Write-Host '      tools above before running build/test/poller commands. Re-run'
        Write-Host '      bash scripts/dev/check-required-tools.sh to confirm.'
        Write-Host ''
    }
}

Invoke-RequiredToolsProbe

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
