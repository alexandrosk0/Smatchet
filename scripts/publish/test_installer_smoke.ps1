param(
    [string]$InstallerPath = "",
    [string]$ReleaseDir = "",
    [string]$ExpectedVersion = "",
    [int]$LaunchSeconds = 5,
    [switch]$CheckSignatures,
    [switch]$ClearUserDataBeforeInstall,
    [switch]$KeepUserDataAfterTest
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-InstallerPath {
    param(
        [string]$Installer,
        [string]$ReleaseRoot
    )

    if (-not [string]::IsNullOrWhiteSpace($Installer)) {
        $full = [System.IO.Path]::GetFullPath($Installer)
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
            throw "Installer not found: $full"
        }
        return $full
    }

    if ([string]::IsNullOrWhiteSpace($ReleaseRoot)) {
        throw "Specify either -InstallerPath or -ReleaseDir."
    }

    $assetsDir = Join-Path ([System.IO.Path]::GetFullPath($ReleaseRoot)) "assets"
    $match = Get-ChildItem -LiteralPath $assetsDir -Filter "*windows-setup.exe" -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $match) {
        throw "Could not find *windows-setup.exe under $assetsDir"
    }
    return $match.FullName
}

function Invoke-SilentUninstallIfPresent {
    param([string]$UninstallRegistryPath)

    if (-not (Test-Path -LiteralPath $UninstallRegistryPath)) {
        return
    }
    $uninstallString = (Get-ItemProperty -LiteralPath $UninstallRegistryPath).UninstallString
    if ([string]::IsNullOrWhiteSpace($uninstallString)) {
        return
    }
    Start-Process -FilePath $uninstallString.Trim('"') -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait
}

function Get-SignatureSummary {
    param([string]$Path)

    $sig = Get-AuthenticodeSignature -FilePath $Path
    return [pscustomobject]@{
        Path = $Path
        Status = [string]$sig.Status
        StatusMessage = $sig.StatusMessage
        Subject = if ($sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { "" }
        Thumbprint = if ($sig.SignerCertificate) { $sig.SignerCertificate.Thumbprint } else { "" }
    }
}

$installer = Resolve-InstallerPath -Installer $InstallerPath -ReleaseRoot $ReleaseDir
$installDir = Join-Path $env:LOCALAPPDATA 'Programs\Smatchet'
$userDataDir = Join-Path $env:LOCALAPPDATA 'Smatchet'
$startMenuLink = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Smatchet.lnk'
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{6A63A3FA-86B2-4574-B0F6-7C8781B10C0F}_is1'

Invoke-SilentUninstallIfPresent -UninstallRegistryPath $uninstallKey
if (Test-Path -LiteralPath $installDir) {
    Remove-Item -LiteralPath $installDir -Recurse -Force
}
if ($ClearUserDataBeforeInstall -and (Test-Path -LiteralPath $userDataDir)) {
    Remove-Item -LiteralPath $userDataDir -Recurse -Force
}

$installerSignature = $null
if ($CheckSignatures) {
    $installerSignature = Get-SignatureSummary -Path $installer
}

Start-Process -FilePath $installer -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait

$installedExe = Join-Path $installDir 'Smatchet.exe'
$installedUninstaller = Join-Path $installDir 'unins000.exe'
$startMenuLinkExistsAfterInstall = Test-Path -LiteralPath $startMenuLink
if (-not (Test-Path -LiteralPath $installedExe -PathType Leaf)) {
    throw "Installed exe missing: $installedExe"
}
if (-not (Test-Path -LiteralPath $installedUninstaller -PathType Leaf)) {
    throw "Installed uninstaller missing: $installedUninstaller"
}
if (-not (Test-Path -LiteralPath $uninstallKey)) {
    throw "Uninstall registry key missing after install: $uninstallKey"
}

$installedVersionJson = & powershell -NoProfile -File (Join-Path $PSScriptRoot 'test_windows_version_info.ps1') `
    -Path $installedExe `
    -ExpectedVersion $ExpectedVersion
if ($LASTEXITCODE -ne 0) {
    throw "Installed exe version-info validation failed."
}
$installedVersion = $installedVersionJson | ConvertFrom-Json

$exeSignature = $null
$uninstallerSignature = $null
if ($CheckSignatures) {
    $exeSignature = Get-SignatureSummary -Path $installedExe
    $uninstallerSignature = Get-SignatureSummary -Path $installedUninstaller
}

$proc = Start-Process -FilePath $installedExe -PassThru
Start-Sleep -Seconds $LaunchSeconds
if (-not $proc.HasExited) {
    Stop-Process -Id $proc.Id -Force
}
Start-Sleep -Seconds 1

$userDataFilesAfterLaunch = @()
if (Test-Path -LiteralPath $userDataDir) {
    $userDataFilesAfterLaunch = @(Get-ChildItem -LiteralPath $userDataDir -Recurse -File | Select-Object -ExpandProperty FullName)
}

Invoke-SilentUninstallIfPresent -UninstallRegistryPath $uninstallKey
Start-Sleep -Seconds 1

$installDirExistsAfterUninstall = Test-Path -LiteralPath $installDir
if ($installDirExistsAfterUninstall) {
    throw "Install directory still exists after uninstall: $installDir"
}
if (Test-Path -LiteralPath $uninstallKey) {
    throw "Uninstall registry key still exists after uninstall: $uninstallKey"
}

$userDataFilesAfterUninstall = @()
if (Test-Path -LiteralPath $userDataDir) {
    $userDataFilesAfterUninstall = @(Get-ChildItem -LiteralPath $userDataDir -Recurse -File | Select-Object -ExpandProperty FullName)
    if (-not $KeepUserDataAfterTest) {
        Remove-Item -LiteralPath $userDataDir -Recurse -Force
    }
}

[pscustomobject]@{
    InstallerPath = $installer
    StartMenuLinkExistsAfterInstall = $startMenuLinkExistsAfterInstall
    InstalledVersionInfo = $installedVersion
    InstallerSignature = $installerSignature
    InstalledExeSignature = $exeSignature
    InstalledUninstallerSignature = $uninstallerSignature
    UserDataDirectory = $userDataDir
    UserDataFilesAfterLaunch = $userDataFilesAfterLaunch
    UserDataFilesAfterUninstall = $userDataFilesAfterUninstall
    InstallDirRemovedAfterUninstall = $true
} | ConvertTo-Json -Depth 6
