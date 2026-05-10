<#.
    Build Smatchet native Win64 DX12 artifacts for Unreal packaging with the
    Visual Studio 2022 generator, then optionally deploy the full plugin tree
    to an Unreal project's Plugins folder.

    This helper intentionally uses an MSVC/link.exe-compatible build because
    Unreal Build Tool links the packaged Win64 third-party libraries with MSVC.
    MinGW/MSYS2 archives are not ABI-compatible here, even if renamed to .lib.

    When CMakeCache.txt already exists for the build directory, skips the
    configure step unless -ForceConfigure.

    Examples:
      .\scripts\package_unreal_plugin_msvc.ps1
      .\scripts\package_unreal_plugin_msvc.ps1 -ProjectRoot "D:\MyGame"
      .\scripts\package_unreal_plugin_msvc.ps1 -PackageOnly
      .\scripts\package_unreal_plugin_msvc.ps1 -ConfigurePreset vs-unreal-msvc -BuildDir build/vs-unreal-msvc
      .\scripts\package_unreal_plugin_msvc.ps1 -ForceConfigure
#>
param(
    [string]$ProjectRoot = "C:\Users\alexk\Documents\Unreal Projects\TestProject",
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Debug",
    [switch]$PackageOnly,
    [string]$ConfigurePreset = "vs-unreal-msvc",
    [string]$BuildDir = "",
    [switch]$ForceConfigure
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$pluginName = "SmatchetImGuiPlugin"
$sourcePluginDir = Join-Path $repoRoot "UnrealPlugins\$pluginName"
$fetchContentBaseDir = Join-Path $repoRoot ".fetchcontent-msvc"

function Reset-CMakeBuildDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathToReset
    )

    $resolvedRepoRoot = [System.IO.Path]::GetFullPath($repoRoot)
    $resolvedBuildDir = [System.IO.Path]::GetFullPath($PathToReset)
    if (-not $resolvedBuildDir.StartsWith($resolvedRepoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove build directory outside the repo root: $resolvedBuildDir"
    }

    if (Test-Path -LiteralPath $resolvedBuildDir -PathType Container) {
        Write-Host "==> Removing stale CMake build directory: $resolvedBuildDir"
        Remove-Item -LiteralPath $resolvedBuildDir -Recurse -Force
    }
}

if (-not (Test-Path -Path $sourcePluginDir -PathType Container)) {
    throw "Source plugin directory not found: $sourcePluginDir"
}

if ($ConfigurePreset -ne "vs-unreal-msvc") {
    throw "Configure preset '$ConfigurePreset' is not valid for Win64 Unreal packaging. Unreal Build Tool links with MSVC/link.exe, so use the MSVC-compatible script default 'vs-unreal-msvc'."
}

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $repoRoot "build\vs-unreal-msvc"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
} else {
    $BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
}
$presetBinaryDir = $BuildDir

Write-Host "==> Repo root: $repoRoot"
Write-Host "==> CMake configure preset: $ConfigurePreset"
Write-Host "==> CMake build directory: $BuildDir"
Write-Host "==> FetchContent base dir: $fetchContentBaseDir"
Write-Host "==> Requested configuration hint: $Configuration"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake not found on PATH. Install CMake or use a shell where the Visual Studio CMake generator is available."
}

$cmakeCacheInPresetDir = Join-Path $presetBinaryDir "CMakeCache.txt"
$expectedGenerator = "Visual Studio 17 2022"
$generatorMismatch = $false
if (Test-Path -LiteralPath $cmakeCacheInPresetDir -PathType Leaf) {
    $generatorLine = Select-String -Path $cmakeCacheInPresetDir -Pattern '^CMAKE_GENERATOR:INTERNAL=' -SimpleMatch:$false |
        Select-Object -First 1
    if ($generatorLine) {
        $cachedGenerator = ($generatorLine.Line -replace '^CMAKE_GENERATOR:INTERNAL=', '').Trim()
        if ($cachedGenerator -ne $expectedGenerator) {
            $generatorMismatch = $true
            Write-Host "==> Existing cache uses generator '$cachedGenerator'; expected '$expectedGenerator'."
        }
    }
}
if ($ForceConfigure -or $generatorMismatch) {
    Reset-CMakeBuildDirectory -PathToReset $presetBinaryDir
}
$skipCMakePreset = (Test-Path -LiteralPath $cmakeCacheInPresetDir) -and (-not $ForceConfigure) -and (-not $generatorMismatch)
if (-not $skipCMakePreset) {
    Write-Host "==> Configuring CMake (Visual Studio 17 2022, x64)..."
    Push-Location $repoRoot
    try {
        & cmake -S $repoRoot -B $presetBinaryDir `
            -G $expectedGenerator -A x64 `
            "-DSMATCHET_WITH_LUA_AUTOMATION=ON" `
            "-DSMATCHET_WITH_MCP=ON" `
            "-DSMATCHET_WITH_AI=OFF" `
            "-DFETCHCONTENT_BASE_DIR=$fetchContentBaseDir" `
            "-DSMATCHET_UNREAL_THIRDPARTY_DIR=$($sourcePluginDir)\ThirdParty\Smatchet"
        if ($LASTEXITCODE -ne 0) {
            throw "cmake configure failed with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }
}

Write-Host "==> Building packaging target SmatchetPackageUnrealLibs_DX12..."
Push-Location $repoRoot
try {
    & cmake --build $presetBinaryDir --config $Configuration --target SmatchetPackageUnrealLibs_DX12
    if ($LASTEXITCODE -ne 0) {
        throw "cmake build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
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
