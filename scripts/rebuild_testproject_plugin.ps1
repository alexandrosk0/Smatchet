param(
    [string]$UProjectPath = "C:\Users\alexk\Documents\Unreal Projects\TestProject\TestProject.uproject",
    [string]$EngineRoot = "",
    [switch]$Release
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-EngineRootFromAssociation {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Association
    )

    $candidateRegistryKeys = @(
        "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Association",
        "HKLM:\SOFTWARE\WOW6432Node\EpicGames\Unreal Engine\$Association"
    )

    foreach ($regKey in $candidateRegistryKeys) {
        try {
            $props = Get-ItemProperty -Path $regKey -ErrorAction Stop
            if ($props.InstalledDirectory -and (Test-Path -Path $props.InstalledDirectory -PathType Container)) {
                return $props.InstalledDirectory
            }
        } catch {
            # Ignore missing/unreadable key and continue.
        }
    }

    $launcherGuess = "C:\Program Files\Epic Games\UE_$Association"
    if (Test-Path -Path $launcherGuess -PathType Container) {
        return $launcherGuess
    }

    return $null
}

if (-not (Test-Path -Path $UProjectPath -PathType Leaf)) {
    throw "UProject not found: $UProjectPath"
}

$uprojectDir = Split-Path -Parent $UProjectPath
$uprojectName = [System.IO.Path]::GetFileNameWithoutExtension($UProjectPath)

$repoRoot = Split-Path -Parent $PSScriptRoot
$deployScript = Join-Path $PSScriptRoot "build_and_deploy_unreal_plugin.ps1"
if (-not (Test-Path -Path $deployScript -PathType Leaf)) {
    throw "Deploy script missing: $deployScript"
}

Write-Host "==> Deploying latest plugin into project..."
if ($Release) {
    & $deployScript -ProjectRoot $uprojectDir -Release
} else {
    & $deployScript -ProjectRoot $uprojectDir
}
if ($LASTEXITCODE -ne 0) {
    throw "Deploy step failed with exit code $LASTEXITCODE"
}

if ([string]::IsNullOrWhiteSpace($EngineRoot)) {
    $uprojectJson = Get-Content -Path $UProjectPath -Raw | ConvertFrom-Json
    $association = [string]$uprojectJson.EngineAssociation
    if ([string]::IsNullOrWhiteSpace($association)) {
        throw "EngineAssociation is missing in $UProjectPath. Pass -EngineRoot explicitly."
    }

    $resolved = Resolve-EngineRootFromAssociation -Association $association
    if (-not $resolved) {
        throw "Could not resolve engine root for association '$association'. Pass -EngineRoot explicitly."
    }
    $EngineRoot = $resolved
}

$buildBat = Join-Path $EngineRoot "Engine\Build\BatchFiles\Build.bat"
if (-not (Test-Path -Path $buildBat -PathType Leaf)) {
    throw "Unreal Build.bat not found: $buildBat"
}

Write-Host "==> Engine root: $EngineRoot"
Write-Host "==> Rebuilding project editor target: $uprojectName"

& $buildBat "$uprojectName" "Win64" "Development" "-Project=$UProjectPath" "-WaitMutex" "-NoHotReloadFromIDE"
if ($LASTEXITCODE -ne 0) {
    throw "Unreal build failed with exit code $LASTEXITCODE"
}

Write-Host "==> Plugin rebuild complete for project:"
Write-Host "    $UProjectPath"
