# Sync canonical agent definitions at agents\*.md into the Claude Code
# auto-discovery mirror at .claude\agents\*.md.
#
# Canonical: edit files under agents\ only. Run this script after any
# canonical edit. The mirror gets a YAML-comment banner warning humans
# not to edit it directly.
#
# Idempotent — running twice produces no diff.

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot

$CanonicalDir = Join-Path $RepoRoot 'agents'
$MirrorDir = Join-Path $RepoRoot '.claude\agents'

if (-not (Test-Path $CanonicalDir)) {
  Write-Error "Canonical agents directory not found at $CanonicalDir"
  exit 1
}

if (-not (Test-Path $MirrorDir)) {
  New-Item -ItemType Directory -Path $MirrorDir | Out-Null
}

$canonicalFiles = Get-ChildItem -Path $CanonicalDir -Filter '*.md' -File

foreach ($src in $canonicalFiles) {
  $dst = Join-Path $MirrorDir $src.Name
  $lines = Get-Content $src.FullName

  $output = New-Object System.Collections.Generic.List[string]
  $injected = $false

  foreach ($line in $lines) {
    if (-not $injected -and $line -eq '---') {
      $output.Add('---') | Out-Null
      $output.Add("# AUTO-GENERATED MIRROR of ../../agents/$($src.Name) — DO NOT EDIT.") | Out-Null
      $output.Add('# Run scripts/sync-agents.sh (or sync-agents.ps1) to regenerate.') | Out-Null
      $injected = $true
      continue
    }
    $output.Add($line) | Out-Null
  }

  Set-Content -Path $dst -Value $output -Encoding utf8
}

# Drop stale mirrors that no longer have a canonical source.
# README.md is intentional documentation about the mirror itself — never managed by sync.
$mirrorFiles = Get-ChildItem -Path $MirrorDir -Filter '*.md' -File
foreach ($dst in $mirrorFiles) {
  if ($dst.Name -eq 'README.md') { continue }
  $canonicalPath = Join-Path $CanonicalDir $dst.Name
  if (-not (Test-Path $canonicalPath)) {
    Write-Host "removing stale mirror: $($dst.FullName)"
    Remove-Item $dst.FullName
  }
}

$count = $canonicalFiles.Count
Write-Host "synced $count agents to $MirrorDir"
