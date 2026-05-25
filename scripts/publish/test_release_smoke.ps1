param(
    [Parameter(Mandatory = $true)]
    [string]$ReleaseDir,
    [string]$ExpectedVersion = "",
    [switch]$CheckInstallerSignatures,
    [switch]$ClearUserDataBeforeInstall,
    [switch]$KeepUserDataAfterTest,
    [switch]$SkipLightStandalone
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Require-File {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file missing: $Path"
    }
}

$releaseRoot = [System.IO.Path]::GetFullPath($ReleaseDir)
$assetsDir = Join-Path $releaseRoot "assets"
if (-not (Test-Path -LiteralPath $assetsDir -PathType Container)) {
    throw "Assets directory not found: $assetsDir"
}

$portableZip = (Get-ChildItem -LiteralPath $assetsDir -Filter "*windows-portable.zip" -File | Select-Object -First 1).FullName
$lightPortableZip = ""
if (-not $SkipLightStandalone) {
    $lightPortableZip = (Get-ChildItem -LiteralPath $assetsDir -Filter "*windows-light-portable.zip" -File | Select-Object -First 1).FullName
}
$installerExe = (Get-ChildItem -LiteralPath $assetsDir -Filter "*windows-setup.exe" -File | Select-Object -First 1).FullName
$pluginZip = (Get-ChildItem -LiteralPath $assetsDir -Filter "*unreal-plugin.zip" -File | Select-Object -First 1).FullName
$fabZip = (Get-ChildItem -LiteralPath $assetsDir -Filter "*fab-submission.zip" -File | Select-Object -First 1).FullName
$sourceZip = (Get-ChildItem -LiteralPath $assetsDir -Filter "*source.zip" -File | Select-Object -First 1).FullName

foreach ($required in @($portableZip, $installerExe, $pluginZip, $fabZip, $sourceZip)) {
    Require-File -Path $required
}
if (-not $SkipLightStandalone) {
    Require-File -Path $lightPortableZip
}

$portableExtractDir = Join-Path $releaseRoot "smoke-portable"
$lightPortableExtractDir = Join-Path $releaseRoot "smoke-portable-light"
$pluginExtractDir = Join-Path $releaseRoot "smoke-plugin"
$fabExtractDir = Join-Path $releaseRoot "smoke-fab"
$sourceExtractDir = Join-Path $releaseRoot "smoke-source"
foreach ($dir in @($portableExtractDir, $lightPortableExtractDir, $pluginExtractDir, $fabExtractDir, $sourceExtractDir)) {
    if (Test-Path -LiteralPath $dir) {
        Remove-Item -LiteralPath $dir -Recurse -Force
    }
}

Expand-Archive -LiteralPath $portableZip -DestinationPath $portableExtractDir
if (-not $SkipLightStandalone) {
    Expand-Archive -LiteralPath $lightPortableZip -DestinationPath $lightPortableExtractDir
}
Expand-Archive -LiteralPath $pluginZip -DestinationPath $pluginExtractDir
Expand-Archive -LiteralPath $fabZip -DestinationPath $fabExtractDir
Expand-Archive -LiteralPath $sourceZip -DestinationPath $sourceExtractDir

$portableExe = Join-Path $portableExtractDir "Smatchet.exe"
$portableVersionJson = & powershell -NoProfile -File (Join-Path $PSScriptRoot 'test_windows_version_info.ps1') `
    -Path $portableExe `
    -ExpectedVersion $ExpectedVersion
if ($LASTEXITCODE -ne 0) {
    throw "Portable exe version-info validation failed."
}
$portableVersion = $portableVersionJson | ConvertFrom-Json
$lightPortableVersion = $null
$lightCliSmoke = $null
if (-not $SkipLightStandalone) {
    $lightPortableExe = Join-Path $lightPortableExtractDir "Smatchet-Light.exe"
    $lightPortableVersionJson = & powershell -NoProfile -File (Join-Path $PSScriptRoot 'test_windows_version_info.ps1') `
        -Path $lightPortableExe `
        -ExpectedVersion $ExpectedVersion `
        -ExpectedFileDescription "Smatchet Light Standalone"
    if ($LASTEXITCODE -ne 0) {
        throw "Light portable exe version-info validation failed."
    }
    $lightPortableVersion = $lightPortableVersionJson | ConvertFrom-Json

    $lightScriptsDir = Join-Path $lightPortableExtractDir "Scripts"
    if (-not (Test-Path -LiteralPath $lightScriptsDir -PathType Container)) {
        throw "Light portable zip must include Scripts/ (Lua automation)."
    }

    $listStdout = & $lightPortableExe cmd commands.list --pretty 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "Smatchet-Light.exe cmd commands.list failed with exit $LASTEXITCODE."
    }
    $listEnvelope = $listStdout | ConvertFrom-Json
    if (-not $listEnvelope.ok) {
        throw "commands.list returned ok:false on light build."
    }
    foreach ($item in $listEnvelope.data.items) {
        $name = [string]$item.name
        if ($name -like "ai.*") {
            throw "Light command catalog must not include AI commands; found '$name'."
        }
    }

    $null = & $lightPortableExe cmd ai.send-once 2>&1
    if ($LASTEXITCODE -ne 2) {
        throw "Expected exit 2 (unknown-command) for ai.send-once on light build; got $LASTEXITCODE."
    }
    $lightCliSmoke = [ordered]@{
        CommandsListExitCode = 0
        AiSendOnceExitCode = 2
        CommandCount = [int]$listEnvelope.data.items.Count
    }
}

$pluginManifestPath = Join-Path $pluginExtractDir "SmatchetImGuiPlugin\SmatchetImGuiPlugin.uplugin"
Require-File -Path $pluginManifestPath
$pluginManifest = Get-Content -LiteralPath $pluginManifestPath -Raw | ConvertFrom-Json
if ($ExpectedVersion -and [string]$pluginManifest.VersionName -ne $ExpectedVersion) {
    throw "Plugin VersionName mismatch. Expected '$ExpectedVersion' but found '$($pluginManifest.VersionName)'."
}

$fabMetadataPath = Join-Path $fabExtractDir "fab-submission.json"
Require-File -Path $fabMetadataPath
$fabMetadata = Get-Content -LiteralPath $fabMetadataPath -Raw | ConvertFrom-Json
if ($ExpectedVersion -and [string]$fabMetadata.version -ne $ExpectedVersion) {
    throw "Fab submission version mismatch. Expected '$ExpectedVersion' but found '$($fabMetadata.version)'."
}

$sourceRoot = Get-ChildItem -LiteralPath $sourceExtractDir -Directory | Select-Object -First 1
if (-not $sourceRoot) {
    throw "Source zip did not expand to a top-level source directory."
}
foreach ($excludedPath in @(".claude", "backlog")) {
    $fullExcludedPath = Join-Path $sourceRoot.FullName $excludedPath
    if (Test-Path -LiteralPath $fullExcludedPath) {
        throw "Source zip should exclude '$excludedPath' but it was present at '$fullExcludedPath'."
    }
}

$installerSmokeArgs = @(
    '-NoProfile',
    '-File', (Join-Path $PSScriptRoot 'test_installer_smoke.ps1'),
    '-InstallerPath', $installerExe,
    '-ExpectedVersion', $ExpectedVersion
)
if ($CheckInstallerSignatures) {
    $installerSmokeArgs += '-CheckSignatures'
}
if ($ClearUserDataBeforeInstall) {
    $installerSmokeArgs += '-ClearUserDataBeforeInstall'
}
if ($KeepUserDataAfterTest) {
    $installerSmokeArgs += '-KeepUserDataAfterTest'
}
$installerSmokeJson = & powershell @installerSmokeArgs
if ($LASTEXITCODE -ne 0) {
    throw "Installer smoke test failed."
}
$installerSmoke = $installerSmokeJson | ConvertFrom-Json

[pscustomobject]@{
    ReleaseDir = $releaseRoot
    PortableVersionInfo = $portableVersion
    LightPortableVersionInfo = $lightPortableVersion
    LightCliSmoke = $lightCliSmoke
    PluginVersion = [string]$pluginManifest.VersionName
    PluginVersionNumber = [int]$pluginManifest.Version
    FabSubmissionVersion = [string]$fabMetadata.version
    FabSubmissionTag = [string]$fabMetadata.tag
    InstallerSmoke = $installerSmoke
} | ConvertTo-Json -Depth 8
