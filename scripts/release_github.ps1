<#.
    Build and package Smatchet release artifacts, then optionally publish assets to GitHub releases.

    Artifacts:
    - Standalone zip (SmatchetStandalone.exe + runtime files)
    - Unreal plugin zip (UnrealPlugins/SmatchetImGuiPlugin packaged tree)
    - Source zip (git archive)

    Examples:
      .\scripts\release_github.ps1 -Tag v1.2.3
      .\scripts\release_github.ps1 -Tag v1.2.3 -Publish -Draft -NotesFile .\RELEASE_NOTES.md
      .\scripts\release_github.ps1 -Tag v1.2.3 -Publish -Clobber
#>
param(
    [string]$Tag = "",
    [string]$ReleaseName = "",
    [string]$Notes = "",
    [string]$NotesFile = "",
    [string]$StandalonePreset = "ninja-release",
    [string]$UnrealBuildPreset = "vs-unreal-dx12-release",
    [string]$OutDir = "",
    [switch]$SkipBuild,
    [switch]$SkipStandalone,
    [switch]$SkipUnreal,
    [switch]$SkipSourceZip,
    [switch]$AllowDirty,
    [switch]$Draft,
    [switch]$Prerelease,
    [switch]$Publish,
    [switch]$Clobber
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

function Resolve-Tool {
    param([Parameter(Mandatory = $true)][string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) {
        throw "Required tool not found on PATH: $Name"
    }
    return $cmd.Source
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
        (Join-Path $BuildDir "SmatchetStandalone.exe"),
        (Join-Path $BuildDir "Release/SmatchetStandalone.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    $found = Get-ChildItem -Path $BuildDir -Filter "SmatchetStandalone.exe" -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $found) {
        throw "Could not find SmatchetStandalone.exe under $BuildDir"
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

$repoRoot = Split-Path -Parent $PSScriptRoot
$presetFile = Join-Path $repoRoot "CMakePresets.json"
$pluginRoot = Join-Path $repoRoot "UnrealPlugins/SmatchetImGuiPlugin"
$licensePath = Join-Path $repoRoot "LICENSE"
$readmePath = Join-Path $repoRoot "README.md"

if (-not (Test-Path -LiteralPath $presetFile -PathType Leaf)) {
    throw "Missing CMakePresets.json at repo root: $presetFile"
}
if (-not (Test-Path -LiteralPath $pluginRoot -PathType Container)) {
    throw "Missing Unreal plugin directory: $pluginRoot"
}

$cmakeExe = Resolve-Tool -Name "cmake"
$gitExe = Resolve-Tool -Name "git"
$ghExe = ""
if ($Publish) {
    $ghExe = Resolve-Tool -Name "gh"
}

if ($Publish -and [string]::IsNullOrWhiteSpace($Tag)) {
    throw "-Tag is required when -Publish is set."
}
if ($Notes -and $NotesFile) {
    throw "Use either -Notes or -NotesFile, not both."
}
if ($NotesFile -and -not (Test-Path -LiteralPath $NotesFile -PathType Leaf)) {
    throw "Notes file not found: $NotesFile"
}

$effectiveTag = $Tag
if ([string]::IsNullOrWhiteSpace($effectiveTag)) {
    $effectiveTag = "dev-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
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
$assetPaths = New-Object System.Collections.Generic.List[string]

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
        Invoke-Checked -FilePath $cmakeExe -Arguments @("--preset", $StandalonePreset)
        Invoke-Checked -FilePath $cmakeExe -Arguments @("--build", "--preset", $StandalonePreset)
    }
    finally {
        Pop-Location
    }

    if (-not $SkipUnreal) {
        Write-Stage "Configuring and building Unreal package target ($UnrealBuildPreset)"
        Push-Location $repoRoot
        try {
            Invoke-Checked -FilePath $cmakeExe -Arguments @("--preset", $UnrealBuildPreset)
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
    Copy-Item -LiteralPath $exePath -Destination (Join-Path $standaloneStage "SmatchetStandalone.exe")

    $exeDir = Split-Path -Parent $exePath
    $dlls = Get-ChildItem -Path $exeDir -Filter "*.dll" -File -ErrorAction SilentlyContinue
    foreach ($dll in $dlls) {
        Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $standaloneStage $dll.Name)
    }

    $scriptsDir = Join-Path $exeDir "Scripts"
    if (Test-Path -LiteralPath $scriptsDir -PathType Container) {
        Copy-Item -LiteralPath $scriptsDir -Destination (Join-Path $standaloneStage "Scripts") -Recurse -Force
    }
    if (Test-Path -LiteralPath $licensePath -PathType Leaf) {
        Copy-Item -LiteralPath $licensePath -Destination (Join-Path $standaloneStage "LICENSE")
    }
    if (Test-Path -LiteralPath $readmePath -PathType Leaf) {
        Copy-Item -LiteralPath $readmePath -Destination (Join-Path $standaloneStage "README.md")
    }

    $standaloneZip = Join-Path $assetsDir "Smatchet-$effectiveTag-windows-standalone.zip"
    Compress-Directory -SourceDir $standaloneStage -ZipPath $standaloneZip
    $assetPaths.Add($standaloneZip)
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

    $pluginZip = Join-Path $assetsDir "Smatchet-$effectiveTag-unreal-plugin.zip"
    Compress-Directory -SourceDir $pluginStage -ZipPath $pluginZip
    $assetPaths.Add($pluginZip)
}

if (-not $SkipSourceZip) {
    Write-Stage "Creating source archive"
    $sourceZip = Join-Path $assetsDir "Smatchet-$effectiveTag-source.zip"
    $sourceRef = "HEAD"
    Push-Location $repoRoot
    try {
        $tagExists = (Invoke-Checked -FilePath $gitExe -Arguments @("rev-parse", "-q", "--verify", "refs/tags/$effectiveTag") -AllowFailure)
        if ($tagExists -eq 0) {
            $sourceRef = "refs/tags/$effectiveTag"
        }
        elseif ($Publish) {
            throw "Publish requested but tag '$effectiveTag' does not exist for source archive ref."
        }

        Invoke-Checked -FilePath $gitExe -Arguments @("archive", "--format=zip", "--prefix=Smatchet-$effectiveTag/", "--output=$sourceZip", $sourceRef)
    }
    finally {
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
