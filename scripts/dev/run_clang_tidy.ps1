<#.
    Run clang-tidy on first-party C++ under Source_Core/ and Plugins/ using a CMake build tree
    that contains compile_commands.json (CMAKE_EXPORT_COMPILE_COMMANDS=ON).

    Prerequisites:
    - LLVM clang-tidy on PATH (same major version as clang++ used for the build is ideal)
    - Configured build: cmake --preset ninja-debug-msvc (default)

    Examples:
      .\scripts\dev\run_clang_tidy.ps1
      .\scripts\dev\run_clang_tidy.ps1 -BuildDir build/ninja-debug-msvc
      .\scripts\dev\run_clang_tidy.ps1 -Checks "-*,clang-analyzer-deadcode.*"
      .\scripts\dev\run_clang_tidy.ps1 -Fix
#>
param(
    [string]$BuildDir = "",
    [string]$ClangTidy = "clang-tidy",
    [string]$Checks = "",
    [switch]$Fix,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

if (-not $BuildDir) {
    $BuildDir = Join-Path $repoRoot "build/ninja-debug-msvc"
}
elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $repoRoot $BuildDir
}

$db = Join-Path $BuildDir "compile_commands.json"
if (-not (Test-Path -LiteralPath $db -PathType Leaf)) {
    throw "Missing compile_commands.json at '$db'. From repo root run: cmake --preset ninja-debug-msvc"
}

$null = Get-Command $ClangTidy -ErrorAction Stop

$coreRoot = Join-Path $repoRoot "Source_Core"
$pluginsRoot = Join-Path $repoRoot "Plugins"
if (-not (Test-Path -LiteralPath $coreRoot -PathType Container)) {
    throw "Expected directory: $coreRoot"
}
if (-not (Test-Path -LiteralPath $pluginsRoot -PathType Container)) {
    throw "Expected directory: $pluginsRoot"
}

$cppFiles = @(
    Get-ChildItem -Path $coreRoot -Filter "*.cpp" -Recurse -File -ErrorAction Stop |
        ForEach-Object { $_.FullName }
    Get-ChildItem -Path $pluginsRoot -Filter "*.cpp" -Recurse -File -ErrorAction Stop |
        ForEach-Object { $_.FullName }
) | Sort-Object -Unique

if ($cppFiles.Count -eq 0) {
    throw "No .cpp files found under Source_Core or Plugins."
}

$tidyArgs = @("-p", $BuildDir)
if ($Checks) {
    $tidyArgs += "-checks=$Checks"
}
if ($Fix) {
    $tidyArgs += "-fix"
}
if ($Quiet) {
    $tidyArgs += "--quiet"
}

Write-Host "clang-tidy: $($cppFiles.Count) file(s), build dir: $BuildDir"

$lastCode = 0
foreach ($src in $cppFiles) {
    Write-Host ""
    Write-Host $src
    & $ClangTidy @tidyArgs $src
    if ($LASTEXITCODE -ne 0) {
        $lastCode = $LASTEXITCODE
    }
}

exit $lastCode
