param(
    [string]$ProjectFile = "C:\Users\alexk\Documents\Unreal Projects\TestProject\TestProject.uproject",
    [string]$BuildDir = "",
    [string]$UnrealBuildBat = "",
    [string]$UnrealPlatform = "Win64",
    [string]$UnrealConfiguration = "Development",
    [switch]$Release,
    [switch]$ForceConfigure
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-PackageConfiguration {
    param(
        [string]$TargetConfiguration,
        [switch]$ForceRelease
    )

    if ($ForceRelease) {
        return "Release"
    }

    if ($TargetConfiguration -eq "Debug") {
        return "Debug"
    }

    # UBT uses the plugin's Development lib folder for Development, DebugGame,
    # Shipping, and other non-Debug Win64 targets. Our CMake packaging target
    # maps every non-Debug config to that Development folder.
    return "Release"
}

function Resolve-UnrealBuildBat {
    param(
        [string]$ExplicitPath
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (-not (Test-Path -Path $ExplicitPath -PathType Leaf)) {
            throw "Unreal Build.bat not found at explicit path: $ExplicitPath"
        }
        return $ExplicitPath
    }

    $fromEnv = $env:UNREAL_BUILD_BAT
    if (-not [string]::IsNullOrWhiteSpace($fromEnv) -and (Test-Path -Path $fromEnv -PathType Leaf)) {
        return $fromEnv
    }

    $epicRoot = "C:\Program Files\Epic Games"
    if (Test-Path -Path $epicRoot -PathType Container) {
        $candidates = Get-ChildItem -Path $epicRoot -Directory -Filter "UE_*" |
            Sort-Object Name -Descending
        foreach ($candidate in $candidates) {
            $buildBat = Join-Path $candidate.FullName "Engine\Build\BatchFiles\Build.bat"
            if (Test-Path -Path $buildBat -PathType Leaf) {
                return $buildBat
            }
        }
    }

    throw "Could not locate Unreal Build.bat. Pass -UnrealBuildBat or set UNREAL_BUILD_BAT."
}

if (-not (Test-Path -Path $ProjectFile -PathType Leaf)) {
    throw "Unreal project file not found: $ProjectFile"
}

$projectRoot = Split-Path -Parent $ProjectFile
$packageScript = Join-Path $PSScriptRoot "package_unreal_plugin_msvc.ps1"
if (-not (Test-Path -Path $packageScript -PathType Leaf)) {
    throw "Package/deploy script not found: $packageScript"
}

$packageArgs = @{
    ProjectRoot = $projectRoot
    PackageOnly = $false
}

if (-not [string]::IsNullOrWhiteSpace($BuildDir)) {
    $packageArgs.BuildDir = $BuildDir
}

$packageArgs.Configuration = Resolve-PackageConfiguration -TargetConfiguration $UnrealConfiguration -ForceRelease:$Release
$packageArgs.ConfigurePreset = "vs-unreal-msvc"

if ($ForceConfigure) {
    $packageArgs.ForceConfigure = $true
}

Write-Host "=====> Packaging and deploying Unreal plugin..."
Write-Host "=====> Native package configuration: $($packageArgs.Configuration) (for Unreal $UnrealConfiguration)"
& $packageScript @packageArgs
if ($LASTEXITCODE -ne 0) {
    throw "Package/deploy script failed with exit code $LASTEXITCODE"
}

$projectName = [System.IO.Path]::GetFileNameWithoutExtension($ProjectFile)
$ubtTarget = "$projectName" + "Editor"
$resolvedBuildBat = Resolve-UnrealBuildBat -ExplicitPath $UnrealBuildBat

Write-Host "=====> Rebuilding Unreal target: $ubtTarget ($UnrealPlatform $UnrealConfiguration)"
Write-Host "=====> Using UBT launcher: $resolvedBuildBat"
& $resolvedBuildBat $ubtTarget $UnrealPlatform $UnrealConfiguration $ProjectFile -WaitMutex

if ($LASTEXITCODE -ne 0) {
    throw "Unreal plugin/project rebuild failed with exit code $LASTEXITCODE"
}

Write-Host "==> Opening Unreal project: $ProjectFile"
Start-Process -FilePath $ProjectFile
