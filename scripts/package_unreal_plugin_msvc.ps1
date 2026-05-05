<#.
    Build Smatchet native Win64 DX12 artifacts with MSVC (ABI-compatible with Unreal Editor),
    run SmatchetPackageUnrealLibs_DX12 (copies .lib/.h into repo:
    UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet/...),

    then optionally deploy the full plugin tree to an Unreal project's Plugins folder.

    Why MSVC: MinGW-built .lib/.a is not safe to link into UnrealEditor-SmatchetImGuiPlugin.dll.
    Use this after changing Source_Core (e.g. SmatchetImGuiHost / diagnostics C API).

    Standalone vs Unreal — same native feature flags:
    Lua / MCP / AI are CMake options (SMATCHET_WITH_LUA_AUTOMATION, SMATCHET_WITH_MCP, SMATCHET_WITH_AI)
    baked into SmatchetCore_DX12 / SmatchetImGuiHost_DX12. Unreal's UBT does not toggle them; the
    packaged .lib files carry whatever you configured in THIS build directory's CMakeCache.
    Use the same SMATCHET_WITH_* values here as for your SmatchetStandalone build (ideally one MSVC
    tree: configure once, build SmatchetStandalone for local tests and SmatchetPackageUnrealLibs_DX12
    for the plugin). If you use two build directories (e.g. MinGW standalone + MSVC Unreal), pass
    the same -D SMATCHET_WITH_* flags to both configures. After changing options, reconfigure or wipe
    the build dir so the cache is not stale. Compare SMATCHET_WITH_* entries in CMakeCache.txt
    across trees if something looks mismatched.

    Prerequisites:
    - CMake 3.24+ on PATH
    - Visual Studio 2022 with "Desktop development with C++" (CMake uses VS generator)

    Examples:
      .\scripts\package_unreal_plugin_msvc.ps1
      .\scripts\package_unreal_plugin_msvc.ps1 -ProjectRoot "D:\MyGame"
      .\scripts\package_unreal_plugin_msvc.ps1 -PackageOnly
      .\scripts\package_unreal_plugin_msvc.ps1 -Configuration Debug
#>
param(
    [string]$ProjectRoot = "C:\Users\alexk\Documents\Unreal Projects\TestProject",
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [switch]$PackageOnly,
    [string]$ConfigurePreset = "vs-unreal-dx12-release",
    [string]$BuildDir = ""
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
    $BuildDir = Join-Path $repoRoot "build/vs-unreal-dx12-release"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $repoRoot $BuildDir
}

Write-Host "==> Repo root: $repoRoot"
Write-Host "==> CMake configure preset: $ConfigurePreset"
Write-Host "==> CMake build directory: $BuildDir"
Write-Host "==> MSVC configuration: $Configuration (packages to ThirdParty lib/Win64/$(if ($Configuration -eq 'Debug') { 'Debug' } else { 'Development' }))"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake not found on PATH. Install CMake or use a Visual Studio / x64 Native Tools prompt where cmake is available."
}

if (-not (Test-Path -Path $BuildDir -PathType Container) -or -not (Test-Path -Path (Join-Path $BuildDir "CMakeCache.txt") -PathType Leaf)) {
    Write-Host "==> Configuring (first time or missing CMakeCache)..."
    # cmake --preset reads CMakePresets.json from the current directory; must run from repo root.
    Push-Location $repoRoot
    try {
        & cmake --preset $ConfigurePreset
        if ($LASTEXITCODE -ne 0) {
            throw "cmake --preset $ConfigurePreset failed with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }
}

Write-Host "==> Building packaging target SmatchetPackageUnrealLibs_DX12..."
& cmake --build $BuildDir --config $Configuration --target SmatchetPackageUnrealLibs_DX12
if ($LASTEXITCODE -ne 0) {
    throw "cmake build failed with exit code $LASTEXITCODE"
}

Write-Host "==> Packaged into: $(Join-Path $sourcePluginDir 'ThirdParty\Smatchet')"

if ($PackageOnly) {
    Write-Host "==> -PackageOnly: skipping copy to Unreal project."
    return
}

if (-not (Test-Path -Path $ProjectRoot -PathType Container)) {
    throw "Target Unreal project folder not found: $ProjectRoot"
}

$projectPluginsDir = Join-Path $ProjectRoot "Plugins"
$destPluginDir = Join-Path $projectPluginsDir $pluginName

if (-not (Test-Path -Path $projectPluginsDir -PathType Container)) {
    New-Item -ItemType Directory -Path $projectPluginsDir | Out-Null
}

if (Test-Path -Path $destPluginDir -PathType Container) {
    Write-Host "==> Removing existing deployed plugin: $destPluginDir"
    Remove-Item -Path $destPluginDir -Recurse -Force
}

Write-Host "==> Copying plugin to: $destPluginDir"
Copy-Item -Path $sourcePluginDir -Destination $destPluginDir -Recurse -Force

Write-Host "==> Done."
Write-Host "    Next: rebuild the plugin/editor (e.g. scripts\rebuild_testproject_plugin.ps1 or Build.bat)."
