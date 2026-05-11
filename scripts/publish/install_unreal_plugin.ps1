param(
    [string]$PluginRoot = "",
    [string]$ProjectRoot = "",
    [string]$EngineRoot = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-ExistingPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        throw "$Description path is required."
    }
    if (-not (Test-Path -LiteralPath $PathValue)) {
        throw "$Description path not found: $PathValue"
    }
    return [System.IO.Path]::GetFullPath($PathValue)
}

function Test-UnrealProjectRoot {
    param([Parameter(Mandatory = $true)][string]$PathValue)
    return @(Get-ChildItem -LiteralPath $PathValue -Filter *.uproject -File -ErrorAction SilentlyContinue).Count -gt 0
}

if ([string]::IsNullOrWhiteSpace($PluginRoot)) {
    $packagedPlugin = Join-Path $PSScriptRoot "SmatchetImGuiPlugin"
    if (Test-Path -LiteralPath $packagedPlugin -PathType Container) {
        $PluginRoot = $packagedPlugin
    } else {
        $PluginRoot = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "UnrealPlugins\SmatchetImGuiPlugin"
    }
}

$resolvedPluginRoot = Resolve-ExistingPath -PathValue $PluginRoot -Description "Plugin root"
$upluginPath = Join-Path $resolvedPluginRoot "SmatchetImGuiPlugin.uplugin"
if (-not (Test-Path -LiteralPath $upluginPath -PathType Leaf)) {
    throw "Plugin root does not contain SmatchetImGuiPlugin.uplugin: $resolvedPluginRoot"
}

$targetCount = 0
if (-not [string]::IsNullOrWhiteSpace($ProjectRoot)) { $targetCount++ }
if (-not [string]::IsNullOrWhiteSpace($EngineRoot)) { $targetCount++ }
if ($targetCount -ne 1) {
    throw "Specify exactly one target: -ProjectRoot or -EngineRoot."
}

if (-not [string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $resolvedProjectRoot = Resolve-ExistingPath -PathValue $ProjectRoot -Description "Project root"
    if (-not (Test-UnrealProjectRoot -PathValue $resolvedProjectRoot)) {
        throw "Project root does not contain a .uproject file: $resolvedProjectRoot"
    }
    $destRoot = Join-Path $resolvedProjectRoot "Plugins"
} else {
    $resolvedEngineRoot = Resolve-ExistingPath -PathValue $EngineRoot -Description "Engine root"
    $enginePluginsRoot = Join-Path $resolvedEngineRoot "Engine\Plugins"
    if (-not (Test-Path -LiteralPath $enginePluginsRoot -PathType Container)) {
        throw "Engine root does not contain Engine\\Plugins: $resolvedEngineRoot"
    }
    $destRoot = Join-Path $enginePluginsRoot "Marketplace"
}

if (-not (Test-Path -LiteralPath $destRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $destRoot | Out-Null
}

$destPluginDir = Join-Path $destRoot "SmatchetImGuiPlugin"
if (Test-Path -LiteralPath $destPluginDir -PathType Container) {
    if (-not $Force) {
        throw "Destination already exists: $destPluginDir. Re-run with -Force to replace it."
    }
    Remove-Item -LiteralPath $destPluginDir -Recurse -Force
}

Copy-Item -LiteralPath $resolvedPluginRoot -Destination $destPluginDir -Recurse -Force
Write-Host "Installed SmatchetImGuiPlugin to: $destPluginDir"
