<#.
    Configure and build the standalone executable.

    When CMakeCache.txt already exists for the preset binaryDir, skips cmake --preset (saves ~seconds
    of reconfigure on every Play). First run or -ForceConfigure runs full configure.

    Examples:
      .\scripts\dev\build_standalone.ps1
      .\scripts\dev\build_standalone.ps1 -Preset ninja-iter-msvc
      .\scripts\dev\build_standalone.ps1 -Preset ninja-iter-unreal-msvc -Target SmatchetPackageUnrealLibs_DX12
      .\scripts\dev\build_standalone.ps1 -ForceConfigure
      .\scripts\dev\build_standalone.ps1 -Preset ninja-publish-msvc -CleanFirst
#>
param(
    [string]$Preset = "ninja-debug-msvc",
    [string]$Target = "SmatchetStandalone",
    [switch]$ForceConfigure,
    [switch]$CleanFirst
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# PS 7+ honors this to prevent native stderr from terminating under Stop. Harmless
# on PS 5.1, which gets the same outcome via Invoke-NativeCommand below.
$PSNativeCommandUseErrorActionPreference = $false

function Invoke-NativeCommand {
    <#
      Run a native command (cmake, ninja, ...) without letting its stderr trip
      $ErrorActionPreference = "Stop". On Windows PowerShell 5.1, every stderr
      line from a native exe is wrapped as a NativeCommandError record, which
      Stop treats as terminating -- so cmake warnings kill the script during
      configure unless we drop to Continue around the invocation. Exit code is
      validated explicitly via $LASTEXITCODE.
    #>
    param(
        [Parameter(Mandatory = $true)]
        [string]$FailureMessage,
        [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $Arguments[0] @($Arguments | Select-Object -Skip 1)
    }
    finally {
        $ErrorActionPreference = $previous
    }

    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage (exit code $LASTEXITCODE)"
    }
}

function Assert-Command {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$InstallHint
    )

    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "$Name not found on PATH. $InstallHint"
    }
}

function Get-VsWherePath {
    $pf86 = ${env:ProgramFiles(x86)}
    if ([string]::IsNullOrWhiteSpace($pf86)) {
        $pf86 = "C:\Program Files (x86)"
    }

    $candidate = Join-Path $pf86 "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return $candidate
    }

    return $null
}

function Test-VisualStudioBuildToolsAvailable {
    if (Get-Command "MSBuild" -ErrorAction SilentlyContinue) {
        return $true
    }

    $vswhere = Get-VsWherePath
    if (-not $vswhere) {
        return $false
    }

    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" 2>$null
    foreach ($path in $msbuild) {
        if (-not [string]::IsNullOrWhiteSpace($path) -and (Test-Path -LiteralPath $path -PathType Leaf)) {
            return $true
        }
    }

    return $false
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

$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
. (Join-Path $PSScriptRoot "..\..\common\SmatchetCMakeCommon.ps1")

Assert-Command -Name "cmake" -InstallHint "Install CMake 3.24+ or run from a shell where CMake is available."

if ($Preset -like "ninja*") {
    Assert-Command -Name "ninja" -InstallHint "Install Ninja or launch from an MSYS2 UCRT64 shell where Ninja is available."
}

if ($Preset -like "*-msys2") {
    throw "$Preset is retired. Use ninja-iter-msvc for MSVC or ninja-iter-clang for clang-cl. MSYS2 is no longer required or proposed for building Smatchet."
}

Push-Location $repoRoot
try {
    $binaryDir = Get-SmatchetConfigurePresetBinaryDir -Root $repoRoot -PresetName $Preset
    $cmakeCache = Join-Path $binaryDir "CMakeCache.txt"
    $cmakeCacheOnDisk = Test-Path -LiteralPath $cmakeCache
    $skipCMakePreset = $cmakeCacheOnDisk -and (-not $ForceConfigure)

    if (-not $skipCMakePreset) {
        Invoke-NativeCommand -FailureMessage "cmake --preset $Preset failed" -Arguments "cmake","--preset",$Preset
    }

    $buildArgs = @("cmake","--build","--preset",$Preset,"--target",$Target)
    if ($CleanFirst) {
        $buildArgs += "--clean-first"
    }
    Invoke-NativeCommand -FailureMessage "cmake --build failed" -Arguments $buildArgs
}
finally {
    Pop-Location
}
