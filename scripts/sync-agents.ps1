# Sync canonical agent + agent-tooling sources into the Claude Code
# auto-discovery / hook mirror under .claude\.
#
# Two source trees are mirrored:
#
#   agents\*.md
#     -> .claude\agents\*.md
#     YAML-comment banner injected inside the `---` frontmatter so
#     Claude Code's parser still sees valid YAML.
#
#   agents\_shared\token-tracking\{*.py, SKILL.md}
#     -> .claude\hooks\agent-token-log.py
#     -> .claude\hooks\agents-statusline.py
#     -> .claude\skills\agent-tokens\SKILL.md
#     Python files get a `#`-comment banner injected right after the
#     shebang. SKILL.md gets the same YAML-comment banner used by
#     agents\*.md.
#
# Canonical: edit files under agents\ only. Mirror is auto-generated
# and never to be edited by hand.
#
# Idempotent - running twice produces no diff.

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BannerDash = [char]0x2014

function Write-Utf8NoBom($Path, $Lines) {
  $encoding = New-Object System.Text.UTF8Encoding -ArgumentList $false
  [System.IO.File]::WriteAllText($Path, (($Lines -join "`n") + "`n"), $encoding)
}

# -----------------------------------------------------------------------------
# Section 1 - agents\*.md  ->  .claude\agents\*.md
# -----------------------------------------------------------------------------

$CanonicalDir = Join-Path $RepoRoot 'agents'
$MirrorDir    = Join-Path $RepoRoot '.claude\agents'

if (-not (Test-Path $CanonicalDir)) {
  Write-Error "Canonical agents directory not found at $CanonicalDir"
  exit 1
}

if (-not (Test-Path $MirrorDir)) {
  New-Item -ItemType Directory -Path $MirrorDir | Out-Null
}

$canonicalFiles = Get-ChildItem -Path $CanonicalDir -Filter '*.md' -File

foreach ($src in $canonicalFiles) {
  $dst   = Join-Path $MirrorDir $src.Name
  $lines = Get-Content -Encoding UTF8 $src.FullName

  $output   = New-Object System.Collections.Generic.List[string]
  $injected = $false

  foreach ($line in $lines) {
    if (-not $injected -and $line -eq '---') {
      $output.Add('---') | Out-Null
      $output.Add("# AUTO-GENERATED MIRROR of ../../agents/$($src.Name) $BannerDash DO NOT EDIT.") | Out-Null
      $output.Add('# Run scripts/sync-agents.sh to regenerate.') | Out-Null
      $injected = $true
      continue
    }
    $output.Add($line) | Out-Null
  }

  Write-Utf8NoBom -Path $dst -Lines $output
}

# Drop stale mirrors that no longer have a canonical source.
# README.md is intentional documentation about the mirror itself - never managed by sync.
$mirrorFiles = Get-ChildItem -Path $MirrorDir -Filter '*.md' -File
foreach ($dst in $mirrorFiles) {
  if ($dst.Name -eq 'README.md') { continue }
  $canonicalPath = Join-Path $CanonicalDir $dst.Name
  if (-not (Test-Path $canonicalPath)) {
    Write-Host "removing stale mirror: $($dst.FullName)"
    Remove-Item $dst.FullName
  }
}

$agentsCount = $canonicalFiles.Count

# -----------------------------------------------------------------------------
# Section 2 - agents\_shared\token-tracking\  ->  .claude\{hooks,skills}\
# -----------------------------------------------------------------------------

$TTDir     = Join-Path $RepoRoot 'agents\_shared\token-tracking'
$HooksDir  = Join-Path $RepoRoot '.claude\hooks'
$SkillsDir = Join-Path $RepoRoot '.claude\skills\agent-tokens'

$ttCount = 0
if (Test-Path $TTDir) {
  if (-not (Test-Path $HooksDir))  { New-Item -ItemType Directory -Path $HooksDir  | Out-Null }
  if (-not (Test-Path $SkillsDir)) { New-Item -ItemType Directory -Path $SkillsDir | Out-Null }

  function Sync-PyMirror($src, $dst, $relName) {
    $lines    = Get-Content -Encoding UTF8 $src
    $output   = New-Object System.Collections.Generic.List[string]
    $injected = $false

    foreach ($line in $lines) {
      if (-not $injected -and $line.StartsWith('#!')) {
        $output.Add($line) | Out-Null
        $output.Add("# AUTO-GENERATED MIRROR of ../../agents/_shared/token-tracking/$relName $BannerDash DO NOT EDIT.") | Out-Null
        $output.Add('# Run scripts/sync-agents.sh to regenerate.') | Out-Null
        $injected = $true
        continue
      }
      $output.Add($line) | Out-Null
    }

    if (-not $injected) {
      # No shebang - prepend banner before everything.
      $output.Insert(0, '# Run scripts/sync-agents.sh to regenerate.')
      $output.Insert(0, "# AUTO-GENERATED MIRROR of ../../agents/_shared/token-tracking/$relName $BannerDash DO NOT EDIT.")
    }

    Write-Utf8NoBom -Path $dst -Lines $output
  }

  foreach ($stem in @('agent-token-log','agents-statusline')) {
    $src = Join-Path $TTDir "$stem.py"
    if (Test-Path $src) {
      $dst = Join-Path $HooksDir "$stem.py"
      Sync-PyMirror -src $src -dst $dst -relName "$stem.py"
      $ttCount++
    }
  }

  $skillSrc = Join-Path $TTDir 'SKILL.md'
  if (Test-Path $skillSrc) {
    $skillDst = Join-Path $SkillsDir 'SKILL.md'
    $lines    = Get-Content -Encoding UTF8 $skillSrc
    $output   = New-Object System.Collections.Generic.List[string]
    $injected = $false
    foreach ($line in $lines) {
      if (-not $injected -and $line -eq '---') {
        $output.Add('---') | Out-Null
        $output.Add("# AUTO-GENERATED MIRROR of ../../../agents/_shared/token-tracking/SKILL.md $BannerDash DO NOT EDIT.") | Out-Null
        $output.Add('# Run scripts/sync-agents.sh to regenerate.') | Out-Null
        $injected = $true
        continue
      }
      $output.Add($line) | Out-Null
    }
    Write-Utf8NoBom -Path $skillDst -Lines $output
    $ttCount++
  }
}

# -----------------------------------------------------------------------------
# Section 3 - agents\_shared\skills\grill-with-docs\  ->  .claude\skills\grill-with-docs\
# -----------------------------------------------------------------------------

$GrillSrc = Join-Path $RepoRoot 'agents\_shared\skills\grill-with-docs'
$GrillDst = Join-Path $RepoRoot '.claude\skills\grill-with-docs'

$grillCount = 0
if (Test-Path $GrillSrc) {
  if (-not (Test-Path $GrillDst)) { New-Item -ItemType Directory -Path $GrillDst | Out-Null }

  foreach ($src in (Get-ChildItem -Path $GrillSrc -Filter '*.md' -File)) {
    $name = $src.Name
    $dst  = Join-Path $GrillDst $name
    $lines = Get-Content -Encoding UTF8 $src.FullName
    $output = New-Object System.Collections.Generic.List[string]

    if ($lines.Count -gt 0 -and $lines[0] -eq '---') {
      # YAML-frontmatter (SKILL.md) - inject inside frontmatter.
      $injected = $false
      foreach ($line in $lines) {
        if (-not $injected -and $line -eq '---') {
          $output.Add('---') | Out-Null
          $output.Add("# AUTO-GENERATED MIRROR of ../../../agents/_shared/skills/grill-with-docs/$name $BannerDash DO NOT EDIT.") | Out-Null
          $output.Add('# Run scripts/sync-agents.sh to regenerate.') | Out-Null
          $injected = $true
          continue
        }
        $output.Add($line) | Out-Null
      }
    } else {
      # Plain markdown - prepend HTML-comment banner.
      $output.Add("<!-- AUTO-GENERATED MIRROR of ../../../agents/_shared/skills/grill-with-docs/$name $BannerDash DO NOT EDIT. -->") | Out-Null
      $output.Add('<!-- Run scripts/sync-agents.sh to regenerate. -->') | Out-Null
      foreach ($line in $lines) { $output.Add($line) | Out-Null }
    }

    Write-Utf8NoBom -Path $dst -Lines $output
    $grillCount++
  }

  # Drop stale mirrors that no longer have a canonical source.
  foreach ($dst in (Get-ChildItem -Path $GrillDst -Filter '*.md' -File)) {
    if (-not (Test-Path (Join-Path $GrillSrc $dst.Name))) {
      Write-Host "removing stale mirror: $($dst.FullName)"
      Remove-Item $dst.FullName -Force
    }
  }
}

Write-Host "synced $agentsCount agents to $MirrorDir + $ttCount token-tracking files to .claude\hooks\ + .claude\skills\agent-tokens\ + $grillCount grill-with-docs files to .claude\skills\grill-with-docs\"
