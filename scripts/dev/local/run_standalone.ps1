<#.
    Run a previously built SmatchetStandalone executable.

    Examples:
      .\scripts\dev\run_standalone.ps1
      .\scripts\dev\run_standalone.ps1 -Preset ninja-debug-msvc
      .\scripts\dev\run_standalone.ps1 -BuildDir build/ninja-debug-msvc
      .\scripts\dev\run_standalone.ps1 -Target MyStandaloneTarget -ExeName MyCustomExe.exe
      .\scripts\dev\run_standalone.ps1 -StandaloneArgs '--config','C:\tmp\smatchet_config.json'
#>
param(
    [string]$Preset = "ninja-debug-msvc",
    [string]$Target = "SmatchetStandalone",
    [string]$ExeName = "",
    [string]$BuildDir = "",
    [string]$Configuration = "",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$StandaloneArgs = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))

function Read-PresetFiles {
    $files = @(
        (Join-Path $repoRoot "CMakePresets.json"),
        (Join-Path $repoRoot "CMakeUserPresets.json")
    )

    $presets = @()
    foreach ($file in $files) {
        if (Test-Path -LiteralPath $file -PathType Leaf) {
            $presets += Get-Content $file -Raw | ConvertFrom-Json
        }
    }
    return $presets
}

function Get-PresetProperty {
    param(
        [Parameter(Mandatory = $true)]
        [object]$PresetObject,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $property = $PresetObject.PSObject.Properties[$Name]
    if ($property) {
        return $property.Value
    }
    return $null
}

function Find-NamedPreset {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$PresetFiles,
        [Parameter(Mandatory = $true)]
        [string]$CollectionName,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    foreach ($presetFile in $PresetFiles) {
        $collection = Get-PresetProperty -PresetObject $presetFile -Name $CollectionName
        if (-not $collection) {
            continue
        }
        foreach ($presetItem in $collection) {
            # CMake preset names are case-sensitive per the JSON schema;
            # use -ceq instead of PowerShell's case-insensitive -eq.
            if ((Get-PresetProperty -PresetObject $presetItem -Name "name") -ceq $Name) {
                return $presetItem
            }
        }
    }
    return $null
}

function Resolve-ConfigurePreset {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$PresetFiles,
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [string[]]$Stack = @()
    )

    # CMake preset names are case-sensitive; use -ccontains to match.
    if ($Stack -ccontains $Name) {
        $cycle = @($Stack + $Name) -join " -> "
        throw "CMake configure preset inheritance cycle detected: $cycle"
    }

    $presetItem = Find-NamedPreset -PresetFiles $PresetFiles -CollectionName "configurePresets" -Name $Name
    if (-not $presetItem) {
        throw "CMake configure preset not found: $Name"
    }

    $resolved = [ordered]@{}
    $inherits = @((Get-PresetProperty -PresetObject $presetItem -Name "inherits"))
    foreach ($parent in $inherits) {
        if ($parent) {
            $parentResolved = Resolve-ConfigurePreset -PresetFiles $PresetFiles -Name $parent -Stack ($Stack + $Name)
            foreach ($property in $parentResolved.PSObject.Properties) {
                $resolved[$property.Name] = $property.Value
            }
        }
    }
    foreach ($property in $presetItem.PSObject.Properties) {
        $resolved[$property.Name] = $property.Value
    }

    return [pscustomobject]$resolved
}

function Resolve-StandaloneBuildLocation {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PresetName
    )

    $presetFiles = Read-PresetFiles
    $buildPreset = Find-NamedPreset -PresetFiles $presetFiles -CollectionName "buildPresets" -Name $PresetName
    $configurePresetName = $PresetName
    $configurationName = $Configuration

    if ($buildPreset) {
        $configurePresetName = Get-PresetProperty -PresetObject $buildPreset -Name "configurePreset"
        $buildPresetConfiguration = Get-PresetProperty -PresetObject $buildPreset -Name "configuration"
        if (-not $configurationName -and $buildPresetConfiguration) {
            $configurationName = $buildPresetConfiguration
        }
    }

    $configurePreset = Resolve-ConfigurePreset -PresetFiles $presetFiles -Name $configurePresetName

    # `condition` blocks gate whether a preset is enabled on this host. We don't
    # evaluate them; if one exists, force the user to be explicit so we never
    # run a build dir that CMake would have skipped.
    $condition = Get-PresetProperty -PresetObject $configurePreset -Name "condition"
    if ($null -ne $condition) {
        throw "Configure preset '$configurePresetName' has a 'condition' block this script does not evaluate. Pass -BuildDir explicitly."
    }

    $binaryDir = Get-PresetProperty -PresetObject $configurePreset -Name "binaryDir"
    if (-not $binaryDir) {
        throw "CMake configure preset '$configurePresetName' does not define binaryDir."
    }

    # This lightweight runner only resolves `${sourceDir}`. Any other CMake
    # preset macro (`${presetName}`, `${generator}`, `$env{...}`, `$penv{...}`,
    # etc.) would silently produce a wrong path, so reject them up front and
    # tell the user to pass -BuildDir explicitly.
    $resolvedBuildDir = $binaryDir.Replace('${sourceDir}', $repoRoot)
    if ($resolvedBuildDir -match '\$(\{[^}]+\}|env\{[^}]+\}|penv\{[^}]+\})') {
        throw "binaryDir '$binaryDir' on preset '$configurePresetName' contains a CMake preset macro this script does not resolve (only `${sourceDir}` is supported). Pass -BuildDir explicitly to bypass."
    }

    return [pscustomobject]@{
        BuildDir = $resolvedBuildDir
        Configuration = $configurationName
    }
}

if (-not $BuildDir) {
    $location = Resolve-StandaloneBuildLocation -PresetName $Preset
    $BuildDir = $location.BuildDir
    $Configuration = $location.Configuration
}

if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $repoRoot $BuildDir
}

if (-not $ExeName) {
    # CMake target SmatchetStandalone now emits Smatchet.exe (OUTPUT_NAME override).
    if ($Target -eq "SmatchetStandalone") {
        $ExeName = "Smatchet.exe"
    } else {
        $ExeName = "$Target.exe"
    }
}
elseif ([System.IO.Path]::GetExtension($ExeName) -eq "") {
    $ExeName = "$ExeName.exe"
}

$exe = if ($Configuration) {
    Join-Path (Join-Path $BuildDir $Configuration) $ExeName
}
else {
    Join-Path $BuildDir $ExeName
}

if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Expected executable missing: $exe"
}

# Print resolved exe path and last-write timestamp before launch.
$exeItem = Get-Item -LiteralPath $exe
Write-Host "  Exe : $($exeItem.FullName)"
Write-Host "  Time: $($exeItem.LastWriteTime)"

# Stale-sibling comparison table — list known sibling outputs, mark the selected one.
$siblingPaths = @(
    (Join-Path $repoRoot "build\ninja-debug-msvc\Smatchet.exe"),
    (Join-Path $repoRoot "build\ninja-iter-msvc\Smatchet.exe"),
    (Join-Path $repoRoot "build\ninja-publish-msvc\Smatchet.exe")
)
# Include caller-provided BuildDir path if it differs from the above.
$callerExe = $exeItem.FullName
$siblingFound = $false
foreach ($s in $siblingPaths) {
    $resolved = Resolve-Path -LiteralPath $s -ErrorAction SilentlyContinue
    if ($resolved -and ($resolved.ProviderPath -ceq $callerExe)) {
        $siblingFound = $true
        break
    }
}
if (-not $siblingFound) {
    $siblingPaths = @($siblingPaths) + @($callerExe)
}

$rows = @()
foreach ($path in $siblingPaths) {
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $item = Get-Item -LiteralPath $path
        $marker = if ($item.FullName -ceq $callerExe) { "<<< selected" } else { "" }
        $rows += [pscustomobject]@{ Path = $item.FullName; LastWriteTime = $item.LastWriteTime; Note = $marker }
    }
}
if ($rows.Count -gt 1) {
    Write-Host ""
    Write-Host "Sibling exe comparison:"
    foreach ($row in $rows) {
        Write-Host ("  [{0}]  {1}  {2}" -f $row.LastWriteTime, $row.Path, $row.Note)
    }
    Write-Host ""
}

# Determine if this is an interactive (GUI) launch or a command/CLI-style invocation.
# CLI-style: first arg starts with '--' or '-' or is a recognised command verb.
# Interactive: no args supplied.
$isCommandStyle = ($StandaloneArgs.Count -gt 0)

if ($isCommandStyle) {
    # Foreground execution — caller gets stdout, stderr, and exit code.
    & $exe @StandaloneArgs
    exit $LASTEXITCODE
} else {
    # Interactive GUI launch — use Start-Process so we can print the PID and
    # return immediately to the caller (the app manages its own window).
    $proc = Start-Process -FilePath $exe -PassThru
    Write-Host "  PID : $($proc.Id)"
}
