# Temporary build helper — MSVC ASAN preset
# Strips MSYS2/MinGW from PATH, loads vcvars64 pinned to the project's MSVC
# toolset (build.msvc_toolset_pin / $env:SMATCHET_VCVARS_VER), then configures +
# builds. The pin avoids loading a newer side-by-side toolset's STL headers
# against the cached cl.exe (error STL1001) — mirrors scripts/dev/with-msvc-env.sh.
param(
    [string]$SourceDir = "C:\Dev\Smatchet"
)

Set-Location $SourceDir

# Strip MSYS2/MinGW entries from PATH so cl.exe wins
$cleanPath = ($env:PATH -split ';') | Where-Object {
    $_ -notmatch 'msys|mingw|ucrt|MSYS2' -and $_ -notmatch '\\msys64\\'
}
$env:PATH = $cleanPath -join ';'

# Resolve the MSVC toolset to pin: $env:SMATCHET_VCVARS_VER overrides
# build.msvc_toolset_pin in project.config.json. Pinning avoids loading a newer
# side-by-side toolset's STL headers against the cached cl.exe (error STL1001).
$vcvarsVer = $env:SMATCHET_VCVARS_VER
if ([string]::IsNullOrWhiteSpace($vcvarsVer)) {
    $configPath = Join-Path $SourceDir "project.config.json"
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
        throw "project.config.json not found at $configPath; set `$env:SMATCHET_VCVARS_VER to an installed MSVC toolset (e.g. 14.38)."
    }
    $cfg = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    $buildProp = $cfg.PSObject.Properties["build"]
    $pinProp = if ($buildProp) { $buildProp.Value.PSObject.Properties["msvc_toolset_pin"] } else { $null }
    if ($pinProp -and -not [string]::IsNullOrWhiteSpace($pinProp.Value)) {
        $vcvarsVer = [string]$pinProp.Value
    } else {
        throw "build.msvc_toolset_pin missing from project.config.json; set `$env:SMATCHET_VCVARS_VER to an installed MSVC toolset (e.g. 14.38)."
    }
}
$vcvarsVer = $vcvarsVer.Trim()

# Pick the first VS install that actually ships the pinned toolset under
# VC\Tools\MSVC\<ver>* — NOT `vswhere -latest`, which on a multi-VS box can
# return a newer BuildTools install that lacks the pinned toolset.
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe not found at $vswhere; install Visual Studio 2017+ (Community is free)."
}
$vsInstall = $null
$installs = & $vswhere -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath 2>$null
foreach ($install in $installs) {
    if ([string]::IsNullOrWhiteSpace($install)) { continue }
    $msvcRoot = Join-Path $install.Trim() "VC\Tools\MSVC"
    if (Test-Path -LiteralPath $msvcRoot -PathType Container) {
        $match = Get-ChildItem -LiteralPath $msvcRoot -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like "$vcvarsVer*" } | Select-Object -First 1
        if ($match) { $vsInstall = $install.Trim(); break }
    }
}
if (-not $vsInstall) {
    throw "No Visual Studio install ships MSVC toolset $vcvarsVer. Install it, or set `$env:SMATCHET_VCVARS_VER to an installed toolset."
}

$vcvarsBat = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
Write-Host "=== vcvars64 ($vsInstall, MSVC $vcvarsVer) ===" -ForegroundColor Cyan

# Load vcvars64 environment via cmd /c, pinned to the resolved toolset.
$vcvarsResult = cmd /c "`"$vcvarsBat`" -vcvars_ver=$vcvarsVer && set" 2>&1
foreach ($line in $vcvarsResult) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}

Write-Host "=== cl.exe version ===" -ForegroundColor Cyan
cl.exe 2>&1 | Select-Object -First 2

Write-Host "=== Configuring ninja-msvc-asan ===" -ForegroundColor Cyan
cmake --preset ninja-msvc-asan -DSMATCHET_ENABLE_STRICT_WARNINGS=ON
if ($LASTEXITCODE -ne 0) {
    Write-Error "Configure FAILED (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

Write-Host "=== Building ninja-msvc-asan ===" -ForegroundColor Cyan
cmake --build --preset ninja-msvc-asan 2>&1
exit $LASTEXITCODE
