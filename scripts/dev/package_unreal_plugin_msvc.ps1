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
      .\scripts\dev\package_unreal_plugin_msvc.ps1
      .\scripts\dev\package_unreal_plugin_msvc.ps1 -ProjectRoot "D:\MyGame"
      .\scripts\dev\package_unreal_plugin_msvc.ps1 -PackageOnly
      .\scripts\dev\package_unreal_plugin_msvc.ps1 -ConfigurePreset vs-unreal-msvc -BuildDir build/vs-unreal-msvc
      .\scripts\dev\package_unreal_plugin_msvc.ps1 -ForceConfigure
      .\scripts\dev\package_unreal_plugin_msvc.ps1 -ToolsetVersion 14.44.35207
      .\scripts\dev\package_unreal_plugin_msvc.ps1 -VsGenerator "Visual Studio 18 2026"
#>
param(
    [string]$ProjectRoot = "C:\Users\alexk\Documents\Unreal Projects\TestProject",
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Debug",
    [switch]$PackageOnly,
    [string]$ConfigurePreset = "vs-unreal-msvc",
    [string]$BuildDir = "",
    [switch]$ForceConfigure,
    # Override the auto-detected CMake generator (e.g. "Visual Studio 18 2026").
    # Default prefers VS 2022 because that is the toolchain Unreal links with.
    [string]$VsGenerator = "",
    # Pin a specific MSVC toolset version (e.g. "14.44.35207") via `-T version=`.
    # Must match the toolset Unreal links the packaged libs with, or the MSVC STL
    # satellite symbols (__std_*) won't resolve at the Unreal link step.
    [string]$ToolsetVersion = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
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

function Reset-StaleFetchContent {
    # FetchContent_MakeAvailable drives a per-dependency "<dep>-subbuild" CMake
    # project that caches its own CMAKE_GENERATOR. When the top-level generator
    # changes (e.g. VS 2022 -> VS 2026 after a toolchain upgrade), resetting the
    # main build dir is not enough: the stale subbuild caches under the
    # FetchContent base dir still pin the old generator and the populate step
    # fails with "Does not match the generator used previously". Drop the
    # per-dependency *-build and *-subbuild dirs (keep *-src to avoid a full
    # re-download).
    param(
        [Parameter(Mandatory = $true)]
        [string]$BaseDir,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedGenerator
    )

    if (-not (Test-Path -LiteralPath $BaseDir -PathType Container)) {
        return
    }

    $resolvedRepoRoot = [System.IO.Path]::GetFullPath($repoRoot)
    $resolvedBaseDir = [System.IO.Path]::GetFullPath($BaseDir)
    if (-not $resolvedBaseDir.StartsWith($resolvedRepoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove FetchContent base dir outside the repo root: $resolvedBaseDir"
    }

    $staleFound = $false
    foreach ($cache in (Get-ChildItem -LiteralPath $resolvedBaseDir -Directory -Filter "*-subbuild" -ErrorAction SilentlyContinue)) {
        $cachePath = Join-Path $cache.FullName "CMakeCache.txt"
        $line = Select-String -Path $cachePath -Pattern '^CMAKE_GENERATOR:INTERNAL=' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($line) {
            $cached = ($line.Line -replace '^CMAKE_GENERATOR:INTERNAL=', '').Trim()
            if ($cached -ne $ExpectedGenerator) {
                $staleFound = $true
                break
            }
        }
    }

    if (-not $staleFound) {
        return
    }

    Write-Host "==> Stale FetchContent generator caches detected; clearing *-build / *-subbuild under: $resolvedBaseDir"
    Get-ChildItem -LiteralPath $resolvedBaseDir -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "*-build" -or $_.Name -like "*-subbuild" } |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force }
}

function Get-CMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CachePath,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if (-not (Test-Path -LiteralPath $CachePath -PathType Leaf)) {
        return $null
    }

    $pattern = "^$([System.Text.RegularExpressions.Regex]::Escape($Name)):[^=]*="
    $line = Select-String -Path $CachePath -Pattern $pattern | Select-Object -First 1
    if (-not $line) {
        return $null
    }
    return ($line.Line -replace $pattern, '').Trim()
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

# Detect the installed Visual Studio version and pick the matching CMake
# generator. Hardcoding "Visual Studio 17 2022" broke on machines upgraded to
# VS 2026 (v18): the v17 generator reports "could not find any instance of
# Visual Studio". But blindly taking the newest install (vswhere -latest) is
# also wrong: Unreal links the packaged Win64 libs with its OWN MSVC toolchain
# (VS 2022 / 14.3x-14.4x for UE 5.7 — VS 2026 / 14.50 is not yet whitelisted).
# Building with a newer toolset emits MSVC STL "satellite" symbols (__std_rotate,
# __std_find_first_not_of_trivial_pos_1, __std_regex_transform_primary_char, ...)
# absent in Unreal's older link-time runtime -> LNK2001/LNK2019.
#
# So: PREFER a VS 2022 instance when one is installed; fall back to the newest VS
# only if no VS 2022 exists. Override with -VsGenerator once Unreal ships support
# for a newer toolchain, and pin the exact toolset with -ToolsetVersion.
function Get-VsGenerator {
    param([string]$Override = "")
    if ($Override) { return $Override }
    $fallback = "Visual Studio 17 2022"
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) { return $fallback }

    # Prefer VS 2022 (major 17) to match Unreal's link toolchain.
    $vs2022 = & $vswhere -version "[17.0,18.0)" -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationVersion 2>$null | Select-Object -First 1
    if ($vs2022) { return "Visual Studio 17 2022" }

    $version = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationVersion 2>$null | Select-Object -First 1
    if (-not $version) { return $fallback }
    $major = ($version -split '\.')[0]
    $yearByMajor = @{ "17" = "2022"; "18" = "2026" }
    $year = $yearByMajor[$major]
    if (-not $year) {
        Write-Host "==> WARN: VS major '$major' (v$version) not in generator map; falling back to '$fallback'."
        return $fallback
    }
    Write-Host "==> WARN: no VS 2022 instance found; using newest '$("Visual Studio $major $year")'. If the Unreal link fails with unresolved __std_* symbols, install the VS 2022 build tools or pass -ToolsetVersion to match Unreal."
    return "Visual Studio $major $year"
}
$expectedGenerator = Get-VsGenerator -Override $VsGenerator
$expectedFeatureCache = [ordered]@{
    "SMATCHET_WITH_LUA_AUTOMATION" = "ON"
    "SMATCHET_WITH_MCP" = "OFF"
    "SMATCHET_WITH_AI" = "OFF"
    "SMATCHET_WITH_WHISPER" = "OFF"
    "SMATCHET_WHISPER_LOCAL_BACKEND" = "OFF"
}
$generatorMismatch = $false
$featureMismatch = $false
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
    foreach ($entry in $expectedFeatureCache.GetEnumerator()) {
        $actual = Get-CMakeCacheValue -CachePath $cmakeCacheInPresetDir -Name $entry.Key
        if ($actual -ne $entry.Value) {
            $featureMismatch = $true
            $actualText = if ($null -eq $actual) { "<missing>" } else { $actual }
            Write-Host "==> Existing cache has $($entry.Key)=$actualText; expected $($entry.Value)."
        }
    }
}
if ($ForceConfigure -or $generatorMismatch -or $featureMismatch) {
    Reset-CMakeBuildDirectory -PathToReset $presetBinaryDir
}
$skipCMakePreset = (Test-Path -LiteralPath $cmakeCacheInPresetDir) -and (-not $ForceConfigure) -and (-not $generatorMismatch) -and (-not $featureMismatch)
if (-not $skipCMakePreset) {
    # Build-dir reset alone misses stale per-dependency subbuild generator
    # caches under the FetchContent base dir; clear those too before configure.
    Reset-StaleFetchContent -BaseDir $fetchContentBaseDir -ExpectedGenerator $expectedGenerator
    $toolsetText = if ($ToolsetVersion) { ", toolset $ToolsetVersion" } else { "" }
    Write-Host "==> Configuring CMake ($expectedGenerator, x64$toolsetText)..."
    $cmakeArgs = @("-S", $repoRoot, "-B", $presetBinaryDir, "-G", $expectedGenerator, "-A", "x64")
    if ($ToolsetVersion) {
        $cmakeArgs += @("-T", "version=$ToolsetVersion")
    }
    $cmakeArgs += @(
        "-DSMATCHET_WITH_LUA_AUTOMATION=ON",
        "-DSMATCHET_WITH_MCP=OFF",
        "-DSMATCHET_WITH_AI=OFF",
        "-DSMATCHET_WITH_WHISPER=OFF",
        "-DSMATCHET_WHISPER_LOCAL_BACKEND=OFF",
        "-DFETCHCONTENT_BASE_DIR=$fetchContentBaseDir",
        "-DSMATCHET_UNREAL_THIRDPARTY_DIR=$($sourcePluginDir)\ThirdParty\Smatchet"
    )
    Push-Location $repoRoot
    try {
        & cmake @cmakeArgs
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
Write-Host "    Next: rebuild the plugin/editor (e.g. scripts\dev\rebuild_testproject_plugin.ps1 or Build.bat)."
