param(
    [string]$ProjectRoot = "C:\Users\alexk\Documents\Unreal Projects\TestProject",
    [string]$BuildDir = "",
    [switch]$Release,
    [switch]$ForceConfigure
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

if (-not (Test-Path -Path $sourcePluginDir -PathType Container)) {
    throw "Source plugin directory not found: $sourcePluginDir"
}

$cmakeConfig = if ($Release) { "Release" } else { "Debug" }
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $buildDirAbs = Join-Path $repoRoot "build\vs-unreal-msvc"
} elseif ([System.IO.Path]::IsPathRooted($BuildDir)) {
    $buildDirAbs = [System.IO.Path]::GetFullPath($BuildDir)
} else {
    $buildDirAbs = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
}
$presetBinaryDir = [System.IO.Path]::GetFullPath($buildDirAbs)

Write-Host "==> Repo root: $repoRoot"
Write-Host "==> Build dir (reference): $buildDirAbs"
Write-Host "==> Configure generator: Visual Studio 17 2022 (x64)"
Write-Host "==> FetchContent base dir: $fetchContentBaseDir"
Write-Host "==> CMake configuration: $cmakeConfig"

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
            "-DFETCHCONTENT_BASE_DIR=$fetchContentBaseDir" `
            "-DSMATCHET_UNREAL_THIRDPARTY_DIR=$($sourcePluginDir)\ThirdParty\Smatchet"
        if ($LASTEXITCODE -ne 0) {
            throw "cmake configure failed with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }
}

Write-Host "==> Building Unreal plugin package target..."
Push-Location $repoRoot
try {
    & cmake --build $presetBinaryDir --config $cmakeConfig --target SmatchetPackageUnrealLibs_DX12
    if ($LASTEXITCODE -ne 0) {
        throw "cmake build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
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
