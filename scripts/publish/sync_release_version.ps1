param(
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PSScriptRoot "..\common\SmatchetCMakeCommon.ps1")

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = Get-SmatchetProjectVersion -Root $repoRoot
}

$parts = Get-SmatchetVersionParts -Version $Version
$cmakePath = Join-Path $repoRoot "CMakeLists.txt"
$upluginPath = Get-SmatchetPluginManifestPath -Root $repoRoot

$cmakeText = Get-Content -LiteralPath $cmakePath -Raw
$updatedCmakeText = [System.Text.RegularExpressions.Regex]::Replace(
    $cmakeText,
    'project\s*\(\s*SmatchetApp\s+VERSION\s+[0-9]+\.[0-9]+\.[0-9]+',
    "project(SmatchetApp VERSION $Version",
    1
)

if ($updatedCmakeText -eq $cmakeText) {
    throw "Failed to update SmatchetApp VERSION in $cmakePath"
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($cmakePath, $updatedCmakeText, $utf8NoBom)
Set-SmatchetPluginManifestVersion -Path $upluginPath -Version $Version

Write-Host "Synchronized release version to $Version"
Write-Host "  CMake:   $cmakePath"
Write-Host "  UPlugin: $upluginPath"
