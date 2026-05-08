<#.
    Configure and build the standalone executable.

    Examples:
      .\scripts\build_standalone.ps1
      .\scripts\build_standalone.ps1 -Preset ninja-msvc-debug
      .\scripts\build_standalone.ps1 -Preset vs-debug
#>
param(
    [string]$Preset = "ninja-debug",
    [string]$Target = "SmatchetStandalone"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

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

$repoRoot = Split-Path -Parent $PSScriptRoot

Assert-Command -Name "cmake" -InstallHint "Install CMake 3.24+ or run from a shell where CMake is available."

if ($Preset -like "ninja*") {
    Assert-Command -Name "ninja" -InstallHint "Install Ninja or choose a Visual Studio preset such as vs-debug."
}

if ($Preset -like "ninja-msvc*") {
    Assert-Command -Name "cl" -InstallHint "Run this from a Visual Studio Developer PowerShell or Developer Command Prompt."
}

if ($Preset -like "ninja-clang*") {
    Assert-Command -Name "clang" -InstallHint "Install LLVM/Clang and ensure clang/clang++ are on PATH, or choose a different preset."
    Assert-Command -Name "clang++" -InstallHint "Install LLVM/Clang and ensure clang/clang++ are on PATH, or choose a different preset."
}

if ($Preset -like "vs-*") {
    if (-not (Test-VisualStudioBuildToolsAvailable)) {
        throw "Visual Studio MSBuild tools not found. Install Visual Studio 2022 with Desktop development with C++, or run from a Visual Studio Developer PowerShell."
    }
}

Push-Location $repoRoot
try {
    & cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) {
        throw "cmake --preset $Preset failed with exit code $LASTEXITCODE"
    }

    & cmake --build --preset $Preset --target $Target
    if ($LASTEXITCODE -ne 0) {
        throw "cmake --build --preset $Preset --target $Target failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
