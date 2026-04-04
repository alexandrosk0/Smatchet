param(
    [string]$ProjectRoot = "C:\Users\alexk\Documents\Unreal Projects\TestProject",
    [string]$BuildDir = "",
    [switch]$Release
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$pluginName = "SmatchetImGuiPlugin"
$sourcePluginDir = Join-Path $repoRoot "UnrealPlugins\$pluginName"

if (-not (Test-Path -Path $sourcePluginDir -PathType Container)) {
    throw "Source plugin directory not found: $sourcePluginDir"
}

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = if ($Release) { "build/ninja-unreal-dx12-release" } else { "build/ninja-unreal-dx12" }
}

$buildDirAbs = if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir
} else {
    Join-Path $repoRoot $BuildDir
}

$preset = if ($Release) { "ninja-unreal-dx12-release" } else { "ninja-unreal-dx12" }

Write-Host "==> Repo root: $repoRoot"
Write-Host "==> Build dir: $buildDirAbs"
Write-Host "==> Configure preset: $preset"

if (-not (Test-Path -Path $buildDirAbs -PathType Container)) {
    Write-Host "==> Build directory missing, running: cmake --preset $preset"
    & cmake --preset $preset
    if ($LASTEXITCODE -ne 0) {
        throw "cmake configure failed with exit code $LASTEXITCODE"
    }
}

Write-Host "==> Building Unreal plugin package target..."
& cmake --build $buildDirAbs --target SmatchetPackageUnrealLibs_DX12
if ($LASTEXITCODE -ne 0) {
    throw "cmake build failed with exit code $LASTEXITCODE"
}

$projectPluginsDir = Join-Path $ProjectRoot "Plugins"
$destPluginDir = Join-Path $projectPluginsDir $pluginName

if (-not (Test-Path -Path $ProjectRoot -PathType Container)) {
    throw "Target Unreal project folder not found: $ProjectRoot"
}

if (-not (Test-Path -Path $projectPluginsDir -PathType Container)) {
    New-Item -ItemType Directory -Path $projectPluginsDir | Out-Null
}

if (Test-Path -Path $destPluginDir -PathType Container) {
    Write-Host "==> Removing existing deployed plugin: $destPluginDir"
    Remove-Item -Path $destPluginDir -Recurse -Force
}

Write-Host "==> Copying plugin to project..."
Copy-Item -Path $sourcePluginDir -Destination $destPluginDir -Recurse -Force

Write-Host "==> Done."
Write-Host "Plugin deployed to: $destPluginDir"
