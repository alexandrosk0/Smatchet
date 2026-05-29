# Shared CMake preset helpers for Smatchet PowerShell scripts.
# Dot-source from nested tooling folders, e.g.:
#   . (Join-Path $PSScriptRoot "..\common\SmatchetCMakeCommon.ps1")

function Get-SmatchetConfigurePresetBinaryDir {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string]$PresetName
    )
    $files = @(
        (Join-Path $Root "CMakePresets.json"),
        (Join-Path $Root "CMakeUserPresets.json")
    )
    foreach ($path in $files) {
        if (-not (Test-Path -LiteralPath $path)) {
            continue
        }
        $doc = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        foreach ($p in $doc.configurePresets) {
            if ((Get-Member -InputObject $p -Name "name" -ErrorAction SilentlyContinue) -and
                ($p.name -ceq $PresetName) -and
                (Get-Member -InputObject $p -Name "binaryDir" -ErrorAction SilentlyContinue) -and
                $p.binaryDir) {
                return ($p.binaryDir -replace '\$\{sourceDir\}', $Root)
            }
        }
    }
    return (Join-Path $Root "build\$PresetName")
}

function Get-SmatchetProjectVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $cmakePath = Join-Path $Root "CMakeLists.txt"
    if (-not (Test-Path -LiteralPath $cmakePath -PathType Leaf)) {
        throw "CMakeLists.txt not found: $cmakePath"
    }

    $match = Select-String -Path $cmakePath -Pattern 'project\s*\(\s*SmatchetApp\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)' |
        Select-Object -First 1
    if (-not $match) {
        throw "Unable to locate project(SmatchetApp VERSION x.y.z) in $cmakePath"
    }

    return $match.Matches[0].Groups[1].Value
}

function Get-SmatchetVersionParts {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Version
    )

    if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
        throw "Version must be semantic major.minor.patch: $Version"
    }

    return [PSCustomObject]@{
        Major = [int]$Matches[1]
        Minor = [int]$Matches[2]
        Patch = [int]$Matches[3]
    }
}

function Convert-SmatchetVersionToTag {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Version
    )

    $null = Get-SmatchetVersionParts -Version $Version
    return "v$Version"
}

function Convert-SmatchetVersionToUnrealVersionNumber {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Version
    )

    $parts = Get-SmatchetVersionParts -Version $Version
    return ($parts.Major * 10000) + ($parts.Minor * 100) + $parts.Patch
}

function Get-SmatchetPluginManifestPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    return (Join-Path $Root "Source/UnrealPlugins\SmatchetImGuiPlugin\SmatchetImGuiPlugin.uplugin")
}

function Get-SmatchetPluginManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Plugin manifest not found: $Path"
    }

    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Set-SmatchetPluginManifestVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Version
    )

    $manifest = Get-SmatchetPluginManifest -Path $Path
    $manifest.Version = Convert-SmatchetVersionToUnrealVersionNumber -Version $Version
    $manifest.VersionName = $Version
    $json = $manifest | ConvertTo-Json -Depth 10
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $json + [Environment]::NewLine, $utf8NoBom)
}

function Test-SmatchetPluginManifestMatchesVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Version
    )

    $manifest = Get-SmatchetPluginManifest -Path $Path
    if ([string]$manifest.VersionName -ne $Version) {
        return $false
    }

    $expectedVersion = Convert-SmatchetVersionToUnrealVersionNumber -Version $Version
    return ([int]$manifest.Version -eq $expectedVersion)
}
