<#.
    Build and package Smatchet release artifacts, then optionally publish assets to GitHub releases.

    Artifacts:
    - Standalone zip (Smatchet.exe + runtime files)
    - Unreal plugin zip (UnrealPlugins/SmatchetImGuiPlugin packaged tree)
    - Source zip (git archive, filtered to exclude internal workflow folders)

    Examples:
      .\scripts\publish\release_github.ps1 -Tag v1.2.3
      .\scripts\publish\release_github.ps1 -Tag v1.2.3 -Sign
      .\scripts\publish\release_github.ps1 -Tag v1.2.3 -Publish -Draft -NotesFile .\RELEASE_NOTES.md
      .\scripts\publish\release_github.ps1 -Tag v1.2.3 -Publish -Clobber
      .\scripts\publish\release_github.ps1 -Tag v1.2.3 -ForceConfigure

    Skips cmake --preset when the preset build dir already has CMakeCache.txt (unless -ForceConfigure).
#>
param(
    [string]$Tag = "",
    [string]$ReleaseName = "",
    [string]$Notes = "",
    [string]$NotesFile = "",
    [string]$StandalonePreset = "ninja-publish-msys2",
    [string]$UnrealBuildPreset = "ninja-publish-msys2",
    [string]$OutDir = "",
    [switch]$SkipBuild,
    [switch]$SkipInstaller,
    [switch]$SkipStandalone,
    [switch]$SkipUnreal,
    [switch]$SkipFabBundle,
    [switch]$SkipSourceZip,
    [switch]$AllowDirty,
    [switch]$Draft,
    [switch]$Prerelease,
    [switch]$Publish,
    [switch]$Clobber,
    [switch]$ForceConfigure,
    [switch]$Sign,
    [string]$SignToolPath = "",
    [string]$SigningCertificatePath = "",
    [string]$SigningCertificatePassword = "",
    [string]$SigningCertificateThumbprint = "",
    [string]$SigningCertificateSubject = "",
    [string]$TimestampUrl = "",
    [switch]$UseMachineCertificateStore
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Write-Stage {
    param([string]$Name)
    Write-Host ""
    Write-Host "==> $Name"
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $false)][string[]]$Arguments = @(),
        [switch]$AllowFailure
    )
    & $FilePath @Arguments
    $exitCode = $LASTEXITCODE
    if (-not $AllowFailure -and $exitCode -ne 0) {
        throw "Command failed ($exitCode): $FilePath $($Arguments -join ' ')"
    }
    return $exitCode
}

function New-CleanDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "Refusing to clean empty path."
    }
    $resolved = [System.IO.Path]::GetFullPath($Path)
    $root = [System.IO.Path]::GetPathRoot($resolved)
    if ($resolved -eq $root) {
        throw "Refusing to clean root path: $resolved"
    }
    if (Test-Path -LiteralPath $resolved) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
    New-Item -ItemType Directory -Path $resolved | Out-Null
    return $resolved
}

function Compress-Directory {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDir,
        [Parameter(Mandatory = $true)][string]$ZipPath
    )
    if (Test-Path -LiteralPath $ZipPath) {
        Remove-Item -LiteralPath $ZipPath -Force
    }
    Compress-Archive -Path (Join-Path $SourceDir '*') -DestinationPath $ZipPath -CompressionLevel Optimal
    if (-not (Test-Path -LiteralPath $ZipPath -PathType Leaf)) {
        throw "Failed to create zip archive: $ZipPath"
    }
}

function Copy-IfPresent {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [switch]$Recurse
    )
    if (-not (Test-Path -LiteralPath $SourcePath)) {
        return
    }
    $parent = Split-Path -Parent $DestinationPath
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    if ($Recurse) {
        Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Recurse -Force
    }
    else {
        Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Force
    }
}

function Remove-IfPresent {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Copy-StandaloneRuntimePayload {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDir,
        [Parameter(Mandatory = $true)][string]$DestinationDir
    )

    $scriptsSource = Join-Path $SourceDir "Scripts"
    if (Test-Path -LiteralPath $scriptsSource -PathType Container) {
        $scriptsDest = Join-Path $DestinationDir "Scripts"
        New-Item -ItemType Directory -Path $scriptsDest -Force | Out-Null

        Get-ChildItem -LiteralPath $scriptsSource -Filter "*.lua" -File -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $scriptsDest $_.Name) -Force
        }

        $artSource = Join-Path $scriptsSource "art"
        if (Test-Path -LiteralPath $artSource -PathType Container) {
            Copy-Item -LiteralPath $artSource -Destination (Join-Path $scriptsDest "art") -Recurse -Force
        }
    }

    Copy-IfPresent -SourcePath (Join-Path $SourceDir "Locales") -DestinationPath (Join-Path $DestinationDir "Locales") -Recurse
    Copy-IfPresent -SourcePath (Join-Path $SourceDir "default_settings.json") -DestinationPath (Join-Path $DestinationDir "default_settings.json")
}

function Resolve-Tool {
    param([Parameter(Mandatory = $true)][string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) {
        throw "Required tool not found on PATH: $Name"
    }
    return $cmd.Source
}

function Resolve-InnoSetupCompiler {
    $cmd = Get-Command "ISCC.exe" -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $candidates = @(
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe",
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    $registryKeys = @(
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*"
    )
    foreach ($registryKey in $registryKeys) {
        $entries = Get-ItemProperty $registryKey -ErrorAction SilentlyContinue |
            Where-Object {
                $displayNameProp = $_.PSObject.Properties["DisplayName"]
                $installLocationProp = $_.PSObject.Properties["InstallLocation"]
                $displayName = if ($displayNameProp) { $displayNameProp.Value } else { $null }
                $installLocation = if ($installLocationProp) { $installLocationProp.Value } else { $null }
                $displayName -like "Inno Setup*" -and -not [string]::IsNullOrWhiteSpace($installLocation)
            }
        foreach ($entry in $entries) {
            $candidate = Join-Path $entry.InstallLocation "ISCC.exe"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return $candidate
            }
        }
    }

    return $null
}

function Resolve-SignTool {
    param([string]$PreferredPath = "")

    if (-not [string]::IsNullOrWhiteSpace($PreferredPath)) {
        $fullPreferred = [System.IO.Path]::GetFullPath($PreferredPath)
        if (Test-Path -LiteralPath $fullPreferred -PathType Leaf) {
            return $fullPreferred
        }
        throw "SignTool path not found: $PreferredPath"
    }

    $cmd = Get-Command "signtool.exe" -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $candidates = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" -Recurse -Filter "signtool.exe" `
        -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
        Sort-Object FullName -Descending
    if ($candidates) {
        return $candidates[0].FullName
    }

    $appCertKit = "C:\Program Files (x86)\Windows Kits\10\App Certification Kit\signtool.exe"
    if (Test-Path -LiteralPath $appCertKit -PathType Leaf) {
        return $appCertKit
    }

    throw "Unable to locate signtool.exe. Install the Windows SDK or pass -SignToolPath."
}

function Quote-InnoValue {
    param([Parameter(Mandatory = $true)][string]$Value)
    return ('$q' + ($Value -replace '"', '$q$q') + '$q')
}

function Get-SigningConfiguration {
    param([string]$PreferredSignToolPath = "")

    $enabled = [bool]$Sign
    if (-not $enabled) {
        return [PSCustomObject]@{
            Enabled = $false
        }
    }

    $config = [PSCustomObject]@{
        Enabled = $true
        SignToolPath = Resolve-SignTool -PreferredPath ($(if ($PreferredSignToolPath) { $PreferredSignToolPath } else { $env:SMATCHET_SIGNTOOL_PATH }))
        CertificatePath = $(if ($SigningCertificatePath) { [System.IO.Path]::GetFullPath($SigningCertificatePath) } elseif ($env:SMATCHET_SIGN_PFX_PATH) { [System.IO.Path]::GetFullPath($env:SMATCHET_SIGN_PFX_PATH) } else { "" })
        CertificatePassword = $(if ($SigningCertificatePassword) { $SigningCertificatePassword } else { $env:SMATCHET_SIGN_PFX_PASSWORD })
        CertificateThumbprint = $(if ($SigningCertificateThumbprint) { $SigningCertificateThumbprint } else { $env:SMATCHET_SIGN_CERT_SHA1 })
        CertificateSubject = $(if ($SigningCertificateSubject) { $SigningCertificateSubject } else { $env:SMATCHET_SIGN_CERT_SUBJECT })
        TimestampUrl = $(if ($TimestampUrl) { $TimestampUrl } elseif ($env:SMATCHET_SIGN_TIMESTAMP_URL) { $env:SMATCHET_SIGN_TIMESTAMP_URL } else { "http://timestamp.digicert.com" })
        UseMachineStore = [bool]($(if ($UseMachineCertificateStore) { $true } elseif ($env:SMATCHET_SIGN_USE_MACHINE_STORE) { @("1","true","yes") -contains $env:SMATCHET_SIGN_USE_MACHINE_STORE.ToLowerInvariant() } else { $false }))
    }

    if ($config.CertificatePath) {
        if (-not (Test-Path -LiteralPath $config.CertificatePath -PathType Leaf)) {
            throw "Signing certificate not found: $($config.CertificatePath)"
        }
    }

    $selectorCount = 0
    if ($config.CertificatePath) { $selectorCount++ }
    if ($config.CertificateThumbprint) { $selectorCount++ }
    if ($config.CertificateSubject) { $selectorCount++ }
    if ($selectorCount -ne 1) {
        throw "Signing requires exactly one certificate selector: PFX path, certificate thumbprint, or certificate subject."
    }

    return $config
}

function Get-SignToolArgumentList {
    param(
        [Parameter(Mandatory = $true)]$Config,
        [Parameter(Mandatory = $true)][string]$FilePath
    )

    $args = New-Object System.Collections.Generic.List[string]
    foreach ($arg in @("sign", "/fd", "SHA256", "/td", "SHA256", "/tr", [string]$Config.TimestampUrl)) {
        $args.Add([string]$arg)
    }
    if ($Config.CertificatePath) {
        $args.Add("/f")
        $args.Add($Config.CertificatePath)
        if ($Config.CertificatePassword) {
            $args.Add("/p")
            $args.Add($Config.CertificatePassword)
        }
    }
    elseif ($Config.CertificateThumbprint) {
        if ($Config.UseMachineStore) {
            $args.Add("/sm")
        }
        $args.Add("/sha1")
        $args.Add($Config.CertificateThumbprint)
    }
    else {
        if ($Config.UseMachineStore) {
            $args.Add("/sm")
        }
        $args.Add("/n")
        $args.Add($Config.CertificateSubject)
    }
    $args.Add($FilePath)
    return $args.ToArray()
}

function Get-InnoSignToolDefinition {
    param(
        [Parameter(Mandatory = $true)]$Config,
        [Parameter(Mandatory = $true)][string]$ProgramDescription
    )

    $parts = New-Object System.Collections.Generic.List[string]
    $parts.Add((Quote-InnoValue -Value $Config.SignToolPath))
    $parts.Add("sign")
    $parts.Add("/fd")
    $parts.Add("SHA256")
    $parts.Add("/td")
    $parts.Add("SHA256")
    $parts.Add("/tr")
    $parts.Add((Quote-InnoValue -Value $Config.TimestampUrl))
    $parts.Add("/d")
    $parts.Add((Quote-InnoValue -Value $ProgramDescription))
    if ($Config.CertificatePath) {
        $parts.Add("/f")
        $parts.Add((Quote-InnoValue -Value $Config.CertificatePath))
        if ($Config.CertificatePassword) {
            $parts.Add("/p")
            $parts.Add((Quote-InnoValue -Value $Config.CertificatePassword))
        }
    }
    elseif ($Config.CertificateThumbprint) {
        if ($Config.UseMachineStore) {
            $parts.Add("/sm")
        }
        $parts.Add("/sha1")
        $parts.Add($Config.CertificateThumbprint)
    }
    else {
        if ($Config.UseMachineStore) {
            $parts.Add("/sm")
        }
        $parts.Add("/n")
        $parts.Add((Quote-InnoValue -Value $Config.CertificateSubject))
    }
    $parts.Add('$f')
    return ($parts -join ' ')
}

function Get-SignablePortableFiles {
    param([Parameter(Mandatory = $true)][string]$Root)

    return Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { @(".exe", ".dll") -contains $_.Extension.ToLowerInvariant() } |
        Sort-Object FullName
}

function Assert-AuthenticodeSignaturePresent {
    param([Parameter(Mandatory = $true)][string]$Path)

    $sig = Get-AuthenticodeSignature -FilePath $Path
    if ($sig.Status -eq [System.Management.Automation.SignatureStatus]::NotSigned -or -not $sig.SignerCertificate) {
        throw "File is not authenticode signed: $Path"
    }
}

function Invoke-Signing {
    param(
        [Parameter(Mandatory = $true)]$Config,
        [Parameter(Mandatory = $true)][string[]]$Paths
    )

    foreach ($path in $Paths) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Cannot sign missing file: $path"
        }
        Invoke-Checked -FilePath $Config.SignToolPath -Arguments (Get-SignToolArgumentList -Config $Config -FilePath $path) | Out-Null
        Assert-AuthenticodeSignaturePresent -Path $path
    }
}

function Get-PresetBinaryDir {
    param(
        [Parameter(Mandatory = $true)][string]$PresetFile,
        [Parameter(Mandatory = $true)][string]$PresetName,
        [Parameter(Mandatory = $true)][string]$SourceDir
    )
    $json = Get-Content -LiteralPath $PresetFile -Raw | ConvertFrom-Json
    $allPresets = @()
    if ($json.configurePresets) {
        $allPresets += $json.configurePresets
    }
    if ($json.buildPresets) {
        foreach ($buildPreset in $json.buildPresets) {
            if ($buildPreset.name -eq $PresetName -and $buildPreset.configurePreset) {
                $PresetName = $buildPreset.configurePreset
                break
            }
        }
    }
    foreach ($preset in $allPresets) {
        if ($preset.name -eq $PresetName) {
            $binaryDir = [string]$preset.binaryDir
            if ([string]::IsNullOrWhiteSpace($binaryDir)) {
                throw "Preset '$PresetName' has no binaryDir."
            }
            return $binaryDir.Replace('${sourceDir}', $SourceDir)
        }
    }
    throw "Preset not found in CMakePresets.json: $PresetName"
}

function Get-StandaloneExePath {
    param(
        [Parameter(Mandatory = $true)][string]$BuildDir
    )
    $candidates = @(
        (Join-Path $BuildDir "Smatchet.exe"),
        (Join-Path $BuildDir "Release/Smatchet.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    $found = Get-ChildItem -Path $BuildDir -Filter "Smatchet.exe" -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $found) {
        throw "Could not find Smatchet.exe under $BuildDir"
    }
    return $found.FullName
}

function Assert-CleanGitTree {
    param(
        [Parameter(Mandatory = $true)][string]$GitExe,
        [Parameter(Mandatory = $true)][bool]$AllowDirtyTree
    )
    $status = & $GitExe status --porcelain
    if ($LASTEXITCODE -ne 0) {
        throw "git status failed."
    }
    if (-not $AllowDirtyTree -and -not [string]::IsNullOrWhiteSpace($status)) {
        throw "Working tree not clean. Commit/stash changes or re-run with -AllowDirty."
    }
}

function Use-Msys2Ucrt64Environment {
    $candidateRoots = New-Object System.Collections.Generic.List[string]

    $prefix = $env:MSYSTEM_PREFIX
    if (-not [string]::IsNullOrWhiteSpace($prefix)) {
        $prefix = $prefix.TrimEnd('\', '/')
        $gccFromPrefix = Join-Path ($prefix -replace '/', '\') "bin\gcc.exe"
        if (Test-Path -LiteralPath $gccFromPrefix -PathType Leaf) {
            $root = Split-Path -Parent (Split-Path -Parent $gccFromPrefix)
            $env:MSYS2_ROOT = $root
            $env:MSYSTEM_PREFIX = ($prefix -replace '\\', '/')
            $env:Path = "$($env:MSYSTEM_PREFIX -replace '/', '\')\bin;$root\usr\bin;$env:Path"
            $env:MSYSTEM = "UCRT64"
            $env:CHERE_INVOKING = "1"
            return
        }
    }

    $gccCommand = Get-Command "gcc.exe" -ErrorAction SilentlyContinue
    if ($gccCommand -and $gccCommand.Source -match '^(.*)[\\/]ucrt64[\\/]bin[\\/]gcc\.exe$') {
        $root = [System.IO.Path]::GetFullPath($Matches[1])
        $env:MSYS2_ROOT = $root
        $env:MSYSTEM_PREFIX = (($root.TrimEnd('\', '/')) + "/ucrt64") -replace '\\', '/'
        $env:Path = "$root\ucrt64\bin;$root\usr\bin;$env:Path"
        $env:MSYSTEM = "UCRT64"
        $env:CHERE_INVOKING = "1"
        return
    }

    if (-not [string]::IsNullOrWhiteSpace($env:MSYS2_ROOT)) {
        $candidateRoots.Add($env:MSYS2_ROOT.TrimEnd('\', '/'))
    }

    $registryKeys = @(
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*"
    )
    foreach ($registryKey in $registryKeys) {
        $entries = Get-ItemProperty $registryKey -ErrorAction SilentlyContinue |
            Where-Object {
                $displayNameProp = $_.PSObject.Properties["DisplayName"]
                $installLocationProp = $_.PSObject.Properties["InstallLocation"]
                $displayName = if ($displayNameProp) { $displayNameProp.Value } else { $null }
                $installLocation = if ($installLocationProp) { $installLocationProp.Value } else { $null }
                $displayName -eq "MSYS2" -and -not [string]::IsNullOrWhiteSpace($installLocation)
            }
        foreach ($entry in $entries) {
            $candidateRoots.Add($entry.InstallLocation.TrimEnd('\', '/'))
        }
    }

    foreach ($root in ($candidateRoots | Select-Object -Unique)) {
        $gccFromRoot = Join-Path $root "ucrt64\bin\gcc.exe"
        if (Test-Path -LiteralPath $gccFromRoot -PathType Leaf) {
            $env:MSYS2_ROOT = $root
            $env:MSYSTEM_PREFIX = (($root) + "/ucrt64") -replace '\\', '/'
            $env:Path = "$root\ucrt64\bin;$root\usr\bin;$env:Path"
            $env:MSYSTEM = "UCRT64"
            $env:CHERE_INVOKING = "1"
            return
        }
    }

    throw "Unable to locate an MSYS2 UCRT64 toolchain. Set MSYS2_ROOT or MSYSTEM_PREFIX, or launch from a shell where UCRT64 gcc.exe is already on PATH."
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PSScriptRoot "..\common\SmatchetCMakeCommon.ps1")
$presetFile = Join-Path $repoRoot "CMakePresets.json"
$pluginRoot = Join-Path $repoRoot "UnrealPlugins/SmatchetImGuiPlugin"
$licensePath = Join-Path $repoRoot "LICENSE"
$readmePath = Join-Path $repoRoot "README.md"
$installerScript = Join-Path $PSScriptRoot "installer\SmatchetStandalone.iss"
$pluginInstallScript = Join-Path $PSScriptRoot "install_unreal_plugin.ps1"
$pluginInstallGuide = Join-Path $PSScriptRoot "INSTALL_UNREAL_PLUGIN.md"
$signingConfig = Get-SigningConfiguration -PreferredSignToolPath $SignToolPath

Use-Msys2Ucrt64Environment

if (-not (Test-Path -LiteralPath $presetFile -PathType Leaf)) {
    throw "Missing CMakePresets.json at repo root: $presetFile"
}
if (-not (Test-Path -LiteralPath $pluginRoot -PathType Container)) {
    throw "Missing Unreal plugin directory: $pluginRoot"
}

$cmakeExe = Resolve-Tool -Name "cmake"
$gitExe = Resolve-Tool -Name "git"
$ghExe = ""
$isccExe = $null
if ($Publish) {
    $ghExe = Resolve-Tool -Name "gh"
}

if ($Notes -and $NotesFile) {
    throw "Use either -Notes or -NotesFile, not both."
}
if ($NotesFile -and -not (Test-Path -LiteralPath $NotesFile -PathType Leaf)) {
    throw "Notes file not found: $NotesFile"
}

$projectVersion = Get-SmatchetProjectVersion -Root $repoRoot
$expectedReleaseTag = Convert-SmatchetVersionToTag -Version $projectVersion
$sourcePluginManifest = Get-SmatchetPluginManifestPath -Root $repoRoot
if (-not (Test-SmatchetPluginManifestMatchesVersion -Path $sourcePluginManifest -Version $projectVersion)) {
    throw "Plugin manifest is out of sync with CMake version $projectVersion. Run scripts\\publish\\sync_release_version.ps1 or update the manifest before releasing."
}

$effectiveTag = $Tag
if ([string]::IsNullOrWhiteSpace($effectiveTag)) {
    $effectiveTag = if ($Publish) { $expectedReleaseTag } else { "$expectedReleaseTag-local-$(Get-Date -Format 'yyyyMMdd-HHmmss')" }
}
if ($Publish -and $effectiveTag -ne $expectedReleaseTag) {
    throw "Publish tag '$effectiveTag' does not match canonical version tag '$expectedReleaseTag'."
}
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $repoRoot "out/releases/$effectiveTag"
}
elseif (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $repoRoot $OutDir
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
$assetsDir = Join-Path $OutDir "assets"
$stagingDir = Join-Path $OutDir "staging"
$standaloneStage = Join-Path $stagingDir "standalone"
$pluginStage = Join-Path $stagingDir "plugin"
$fabStage = Join-Path $stagingDir "fab-submission"
$assetPaths = New-Object System.Collections.Generic.List[string]

if (-not $SkipStandalone -and -not $SkipInstaller) {
    $isccExe = Resolve-InnoSetupCompiler
    if (-not $isccExe) {
        throw "Inno Setup 6 compiler (ISCC.exe) is required for installer packaging. Install Inno Setup or re-run with -SkipInstaller."
    }
}

Write-Stage "Validating git context"
Push-Location $repoRoot
try {
    Invoke-Checked -FilePath $gitExe -Arguments @("rev-parse", "--is-inside-work-tree") | Out-Null
    Assert-CleanGitTree -GitExe $gitExe -AllowDirtyTree ([bool]$AllowDirty)

    if ($Publish) {
        Invoke-Checked -FilePath $ghExe -Arguments @("auth", "status")
        Invoke-Checked -FilePath $ghExe -Arguments @("repo", "view")

        $tagCheckCode = Invoke-Checked -FilePath $gitExe -Arguments @("rev-parse", "-q", "--verify", "refs/tags/$effectiveTag") -AllowFailure
        if ($tagCheckCode -ne 0) {
            throw "Tag '$effectiveTag' not found locally. Create and push tag before publish."
        }
    }
}
finally {
    Pop-Location
}

Write-Stage "Preparing output directories"
New-CleanDirectory -Path $OutDir | Out-Null
New-CleanDirectory -Path $assetsDir | Out-Null
New-CleanDirectory -Path $stagingDir | Out-Null

$standaloneBuildDir = [System.IO.Path]::GetFullPath((Get-PresetBinaryDir -PresetFile $presetFile -PresetName $StandalonePreset -SourceDir $repoRoot))
$unrealBuildDir = [System.IO.Path]::GetFullPath((Get-PresetBinaryDir -PresetFile $presetFile -PresetName $UnrealBuildPreset -SourceDir $repoRoot))

if (-not $SkipBuild) {
    Write-Stage "Configuring and building standalone ($StandalonePreset)"
    Push-Location $repoRoot
    try {
        $standaloneCache = Join-Path $standaloneBuildDir "CMakeCache.txt"
        if (-not ((Test-Path -LiteralPath $standaloneCache) -and -not $ForceConfigure)) {
            Invoke-Checked -FilePath $cmakeExe -Arguments @("--preset", $StandalonePreset)
        }
        Invoke-Checked -FilePath $cmakeExe -Arguments @("--build", "--preset", $StandalonePreset)
    }
    finally {
        Pop-Location
    }

    if (-not $SkipUnreal -and $UnrealBuildPreset -ne $StandalonePreset) {
        Write-Stage "Configuring and building Unreal package target ($UnrealBuildPreset)"
        Push-Location $repoRoot
        try {
            $unrealCache = Join-Path $unrealBuildDir "CMakeCache.txt"
            if (-not ((Test-Path -LiteralPath $unrealCache) -and -not $ForceConfigure)) {
                Invoke-Checked -FilePath $cmakeExe -Arguments @("--preset", $UnrealBuildPreset)
            }
            Invoke-Checked -FilePath $cmakeExe -Arguments @("--build", "--preset", $UnrealBuildPreset)
        }
        finally {
            Pop-Location
        }
    }
}

if (-not $SkipStandalone) {
    Write-Stage "Staging standalone artifact"
    New-CleanDirectory -Path $standaloneStage | Out-Null
    $exePath = Get-StandaloneExePath -BuildDir $standaloneBuildDir
    Copy-Item -LiteralPath $exePath -Destination (Join-Path $standaloneStage "Smatchet.exe")

    $exeDir = Split-Path -Parent $exePath
    $dlls = Get-ChildItem -Path $exeDir -Filter "*.dll" -File -ErrorAction SilentlyContinue
    foreach ($dll in $dlls) {
        Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $standaloneStage $dll.Name)
    }

    Copy-StandaloneRuntimePayload -SourceDir $exeDir -DestinationDir $standaloneStage
    if (Test-Path -LiteralPath $licensePath -PathType Leaf) {
        Copy-Item -LiteralPath $licensePath -Destination (Join-Path $standaloneStage "LICENSE")
    }
    if (Test-Path -LiteralPath $readmePath -PathType Leaf) {
        Copy-Item -LiteralPath $readmePath -Destination (Join-Path $standaloneStage "README.md")
    }

    if ($signingConfig.Enabled) {
        Write-Stage "Signing standalone payload"
        $portableSignables = @(Get-SignablePortableFiles -Root $standaloneStage | ForEach-Object { $_.FullName })
        if ($portableSignables.Count -gt 0) {
            Invoke-Signing -Config $signingConfig -Paths $portableSignables
        }
    }

    $standaloneZip = Join-Path $assetsDir "Smatchet-$effectiveTag-windows-portable.zip"
    Compress-Directory -SourceDir $standaloneStage -ZipPath $standaloneZip
    $assetPaths.Add($standaloneZip)

    if (-not $SkipInstaller) {
        Write-Stage "Building Windows installer"
        $installerBaseName = "Smatchet-$effectiveTag-windows-setup"
        Invoke-Checked -FilePath $isccExe -Arguments @(
            "/DMyAppVersion=$projectVersion",
            "/DMySourceDir=$standaloneStage",
            "/DMyOutputDir=$assetsDir",
            "/DMyOutputBaseFilename=$installerBaseName",
            $(if ($signingConfig.Enabled) { "/DMyInnoSignTool=smatchetsigntool" }),
            $(if ($signingConfig.Enabled) { "/Ssmatchetsigntool=$(Get-InnoSignToolDefinition -Config $signingConfig -ProgramDescription 'Smatchet Installer')" }),
            $installerScript
        ) | Out-Null
        $installerPath = Join-Path $assetsDir "$installerBaseName.exe"
        if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
            throw "Expected installer missing: $installerPath"
        }
        if ($signingConfig.Enabled) {
            Assert-AuthenticodeSignaturePresent -Path $installerPath
        }
        $assetPaths.Add($installerPath)
    }
}

if (-not $SkipUnreal) {
    Write-Stage "Staging Unreal plugin artifact"
    New-CleanDirectory -Path $pluginStage | Out-Null

    $pluginDest = Join-Path $pluginStage "SmatchetImGuiPlugin"
    Copy-Item -LiteralPath $pluginRoot -Destination $pluginDest -Recurse -Force

    foreach ($noise in @("Binaries", "Intermediate", "Saved", ".vs")) {
        $noisePath = Join-Path $pluginDest $noise
        if (Test-Path -LiteralPath $noisePath) {
            Remove-Item -LiteralPath $noisePath -Recurse -Force
        }
    }

    $stagedPluginManifest = Join-Path $pluginDest "SmatchetImGuiPlugin.uplugin"
    Set-SmatchetPluginManifestVersion -Path $stagedPluginManifest -Version $projectVersion
    Copy-Item -LiteralPath $pluginInstallScript -Destination (Join-Path $pluginStage "install_unreal_plugin.ps1")
    Copy-Item -LiteralPath $pluginInstallGuide -Destination (Join-Path $pluginStage "INSTALL_UNREAL_PLUGIN.md")

    $pluginZip = Join-Path $assetsDir "Smatchet-$effectiveTag-unreal-plugin.zip"
    Compress-Directory -SourceDir $pluginStage -ZipPath $pluginZip
    $assetPaths.Add($pluginZip)

    if (-not $SkipFabBundle) {
        Write-Stage "Preparing Fab submission bundle"
        New-CleanDirectory -Path $fabStage | Out-Null
        Copy-Item -LiteralPath $pluginZip -Destination (Join-Path $fabStage (Split-Path -Leaf $pluginZip))
        Copy-Item -LiteralPath $pluginInstallGuide -Destination (Join-Path $fabStage "INSTALL_UNREAL_PLUGIN.md")

        $fabMetadata = [ordered]@{
            product = "Smatchet"
            version = $projectVersion
            tag = $expectedReleaseTag
            releaseAsset = Split-Path -Leaf $pluginZip
            pluginFolder = "SmatchetImGuiPlugin"
            pluginManifest = "SmatchetImGuiPlugin/SmatchetImGuiPlugin.uplugin"
            support = @{
                readme = "README.md"
                installGuide = "INSTALL_UNREAL_PLUGIN.md"
            }
            submissionChecklist = @(
                "Upload the plugin zip to Fab publisher portal.",
                "Verify listing version matches the canonical Smatchet version.",
                "Attach release notes and support links.",
                "Confirm the packaged plugin installs into a project Plugins folder."
            )
        } | ConvertTo-Json -Depth 6
        $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText((Join-Path $fabStage "fab-submission.json"), $fabMetadata + [Environment]::NewLine, $utf8NoBom)

        $fabNotes = @"
Fab Submission Bundle
=====================

Product: Smatchet
Version: $projectVersion
Tag: $expectedReleaseTag

Included files:
- $(Split-Path -Leaf $pluginZip)
- INSTALL_UNREAL_PLUGIN.md
- fab-submission.json

Portal steps:
1. Upload the plugin zip to the Fab listing for Smatchet.
2. Set the listing version to $projectVersion.
3. Paste the release notes or link back to the GitHub release for this version.
4. Submit for review after verifying install into a clean Unreal project.
"@
        [System.IO.File]::WriteAllText((Join-Path $fabStage "FAB_SUBMISSION.md"), $fabNotes, $utf8NoBom)

        $fabZip = Join-Path $assetsDir "Smatchet-$effectiveTag-fab-submission.zip"
        Compress-Directory -SourceDir $fabStage -ZipPath $fabZip
        $assetPaths.Add($fabZip)
    }
}

if (-not $SkipSourceZip) {
    Write-Stage "Creating source archive"
    $sourceZip = Join-Path $assetsDir "Smatchet-$effectiveTag-source.zip"
    $sourceRef = "HEAD"
    $sourceStageRoot = Join-Path $OutDir "stage-source"
    $sourceArchiveTemp = Join-Path $OutDir "source-raw.zip"
    $sourcePrefixDir = Join-Path $sourceStageRoot "Smatchet-$effectiveTag"
    $sourceExcludePaths = @(
        ".claude",
        "backlog"
    )
    Push-Location $repoRoot
    try {
        $tagExists = (Invoke-Checked -FilePath $gitExe -Arguments @("rev-parse", "-q", "--verify", "refs/tags/$effectiveTag") -AllowFailure)
        if ($tagExists -eq 0) {
            $sourceRef = "refs/tags/$effectiveTag"
        }
        elseif ($Publish) {
            throw "Publish requested but tag '$effectiveTag' does not exist for source archive ref."
        }

        New-CleanDirectory -Path $sourceStageRoot | Out-Null
        Remove-IfPresent -Path $sourceArchiveTemp

        Invoke-Checked -FilePath $gitExe -Arguments @("archive", "--format=zip", "--prefix=Smatchet-$effectiveTag/", "--output=$sourceArchiveTemp", $sourceRef)
        Expand-Archive -LiteralPath $sourceArchiveTemp -DestinationPath $sourceStageRoot -Force

        foreach ($excludePath in $sourceExcludePaths) {
            Remove-IfPresent -Path (Join-Path $sourcePrefixDir $excludePath)
        }

        Compress-Directory -SourceDir $sourceStageRoot -ZipPath $sourceZip
    }
    finally {
        Remove-IfPresent -Path $sourceArchiveTemp
        Pop-Location
    }
    $assetPaths.Add($sourceZip)
}

foreach ($asset in $assetPaths) {
    if (-not (Test-Path -LiteralPath $asset -PathType Leaf)) {
        throw "Expected asset missing: $asset"
    }
}

if ($Publish) {
    Write-Stage "Publishing assets to GitHub release"
    Push-Location $repoRoot
    try {
        $releaseExists = (Invoke-Checked -FilePath $ghExe -Arguments @("release", "view", $effectiveTag, "--json", "url") -AllowFailure) -eq 0

        if (-not $releaseExists) {
            $createArgs = New-Object System.Collections.Generic.List[string]
            $createArgs.AddRange(@("release", "create", $effectiveTag))
            foreach ($asset in $assetPaths) {
                $createArgs.Add($asset)
            }
            if (-not [string]::IsNullOrWhiteSpace($ReleaseName)) {
                $createArgs.Add("--title")
                $createArgs.Add($ReleaseName)
            }
            if ($NotesFile) {
                $createArgs.Add("--notes-file")
                $createArgs.Add([System.IO.Path]::GetFullPath($NotesFile))
            }
            elseif ($Notes) {
                $createArgs.Add("--notes")
                $createArgs.Add($Notes)
            }
            else {
                $createArgs.Add("--generate-notes")
            }
            if ($Draft) { $createArgs.Add("--draft") }
            if ($Prerelease) { $createArgs.Add("--prerelease") }
            Invoke-Checked -FilePath $ghExe -Arguments $createArgs.ToArray()
        }
        else {
            if (-not $Clobber) {
                throw "Release '$effectiveTag' already exists. Re-run with -Clobber to overwrite assets."
            }
            $uploadArgs = New-Object System.Collections.Generic.List[string]
            $uploadArgs.AddRange(@("release", "upload", $effectiveTag))
            foreach ($asset in $assetPaths) {
                $uploadArgs.Add($asset)
            }
            $uploadArgs.Add("--clobber")
            Invoke-Checked -FilePath $ghExe -Arguments $uploadArgs.ToArray()
        }

        $releaseUrl = & $ghExe release view $effectiveTag --json url --jq .url
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to fetch release URL."
        }
        Write-Host "Release URL: $releaseUrl"
    }
    finally {
        Pop-Location
    }
}

Write-Stage "Done"
Write-Host "Output directory: $OutDir"
if ($assetPaths.Count -gt 0) {
    Write-Host "Assets:"
    foreach ($asset in $assetPaths) {
        Write-Host "  - $asset"
    }
}
