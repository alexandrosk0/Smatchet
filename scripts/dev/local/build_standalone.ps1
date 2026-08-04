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

$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
. (Join-Path $PSScriptRoot "..\..\common\SmatchetCMakeCommon.ps1")

Assert-Command -Name "cmake" -InstallHint "Install CMake 3.24+ or run from a shell where CMake is available."

# Retirement check FIRST: ninja-iter-msys2 also matches the `ninja*` glob below, so a
# host without ninja on PATH would otherwise get "install Ninja" instead of the
# migration hint (and test-build-wrapper.ps1 test 1 asserts on that hint).
if ($Preset -like "*-msys2") {
    throw "$Preset is retired. Use ninja-iter-msvc for MSVC or ninja-iter-clang for clang-cl. MSYS2 is no longer required or proposed for building Smatchet."
}

if ($Preset -like "ninja*") {
    Assert-Command -Name "ninja" -InstallHint "Install Ninja (winget install Ninja-build.Ninja) or run .\build.ps1 from the repo root."
}

# Fail fast with an actionable message instead of letting CMake die later on an opaque
# "CMAKE_CXX_COMPILER not set". Every msvc/clang preset needs the MSVC environment
# (clang-cl still resolves the Windows SDK through it), and cl.exe on PATH is its marker.
# The match is deliberately a substring, not a suffix: the toolchain token sits mid-name in
# ninja-msvc-asan / ninja-clang-asan / ninja-iter-msvc-arm64 / ninja-publish-msvc-arm64, and a
# "*-msvc" suffix test let all four skip the pre-flight. No non-MSVC preset carries either
# token (the Linux/Android ones are *-linux / android-ndk-arm64 / posix-core-check).
if (($Preset -like "*msvc*" -or $Preset -like "*clang*") -and -not (Get-Command "cl.exe" -ErrorAction SilentlyContinue)) {
    throw "$Preset needs the MSVC environment, but cl.exe is not on PATH. Run .\build.ps1 from the repo root (it imports the environment for you), or wrap this command in scripts\dev\with-msvc.ps1."
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
