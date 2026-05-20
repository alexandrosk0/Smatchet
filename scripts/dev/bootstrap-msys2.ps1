<#.
    One-shot Smatchet bootstrap for a fresh Windows machine.

    Installs MSYS2 (via winget), the UCRT64 GCC toolchain + lld + ninja + cmake
    (via pacman), injects MSYS2 paths into the current PowerShell session, then
    runs doctor.sh via the freshly-installed MSYS2 bash. Idempotent -- safe to
    re-run; already-installed packages are skipped.

    Examples:
      .\scripts\dev\bootstrap-msys2.ps1
      .\scripts\dev\bootstrap-msys2.ps1 -IncludeLintTools     # also installs clang-tools-extra, cppcheck
      .\scripts\dev\bootstrap-msys2.ps1 -SkipDoctor

    After this completes successfully you can immediately run:
      cmake --preset ninja-iter-msys2
      cmake --build --preset ninja-iter-msys2
#>
param(
    [switch]$IncludeLintTools,
    [switch]$SkipDoctor
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

function Write-Step  { param([string]$Message) Write-Host "==> $Message" -ForegroundColor Cyan }
function Write-Ok    { param([string]$Message) Write-Host "[ OK ] $Message" -ForegroundColor Green }
function Write-Warn2 { param([string]$Message) Write-Host "[WARN] $Message" -ForegroundColor Yellow }

function Invoke-NativeQuiet {
    # Native invoke with local "Continue" so stderr doesn't terminate; check $LASTEXITCODE explicitly.
    param(
        [Parameter(Mandatory = $true)] [string]$FailureMessage,
        [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)] [string[]]$Arguments
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Arguments[0] @($Arguments | Select-Object -Skip 1) }
    finally { $ErrorActionPreference = $previous }
    if ($LASTEXITCODE -ne 0) { throw "$FailureMessage (exit code $LASTEXITCODE)" }
}

function Find-Msys2Root {
    # Already-set env wins.
    if (-not [string]::IsNullOrWhiteSpace($env:MSYS2_ROOT) -and
        (Test-Path -LiteralPath (Join-Path $env:MSYS2_ROOT "usr\bin\bash.exe"))) {
        return $env:MSYS2_ROOT.TrimEnd('\','/')
    }

    # Conventional locations.
    foreach ($candidate in @('C:\msys64','C:\tools\msys64','D:\msys64')) {
        if (Test-Path -LiteralPath (Join-Path $candidate "usr\bin\bash.exe")) { return $candidate }
    }

    # Registry uninstall entries.
    $keys = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    foreach ($key in $keys) {
        $entries = Get-ItemProperty $key -ErrorAction SilentlyContinue |
            Where-Object {
                $_.PSObject.Properties['DisplayName'] -and $_.DisplayName -eq 'MSYS2' -and
                $_.PSObject.Properties['InstallLocation'] -and -not [string]::IsNullOrWhiteSpace($_.InstallLocation)
            }
        foreach ($entry in $entries) {
            $root = $entry.InstallLocation.TrimEnd('\','/')
            if (Test-Path -LiteralPath (Join-Path $root "usr\bin\bash.exe")) { return $root }
        }
    }

    return $null
}

function Install-Msys2ViaWinget {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "winget not found. Install App Installer from the Microsoft Store, or install MSYS2 manually from https://www.msys2.org/."
    }
    Write-Step "Installing MSYS2 via winget"
    Invoke-NativeQuiet -FailureMessage "winget install MSYS2.MSYS2 failed" `
        -Arguments "winget","install","MSYS2.MSYS2","--accept-package-agreements","--accept-source-agreements","--silent"
}

function Invoke-Pacman {
    param(
        [Parameter(Mandatory = $true)] [string]$Msys2Root,
        [Parameter(Mandatory = $true)] [string[]]$Packages
    )
    $bash = Join-Path $Msys2Root "usr\bin\bash.exe"
    $pkgList = $Packages -join ' '
    # Merge stderr into stdout *inside bash* so pacman's "is up to date" warnings
    # don't surface as PowerShell NativeCommandError records on PS 5.1.
    Invoke-NativeQuiet -FailureMessage "pacman -S $pkgList failed" `
        -Arguments $bash,"-lc","pacman -S --noconfirm --needed $pkgList 2>&1"
}

function Add-Msys2ToSessionPath {
    param([Parameter(Mandatory = $true)] [string]$Msys2Root)
    $ucrt = Join-Path $Msys2Root "ucrt64\bin"
    $usr  = Join-Path $Msys2Root "usr\bin"
    $pathEntries = $env:Path.Split(';')
    foreach ($dir in @($ucrt, $usr)) {
        if ($pathEntries -notcontains $dir) {
            $env:Path = "$dir;$env:Path"
        }
    }
    $env:MSYS2_ROOT = $Msys2Root
    $env:MSYSTEM    = "UCRT64"
    $env:MSYSTEM_PREFIX = ($Msys2Root + "/ucrt64") -replace '\\','/'
    $env:CHERE_INVOKING = "1"
}

# ---------------------------------------------------------------------------

$packages = @(
    'mingw-w64-ucrt-x86_64-toolchain',
    'mingw-w64-ucrt-x86_64-lld',
    'mingw-w64-ucrt-x86_64-ninja',
    'mingw-w64-ucrt-x86_64-cmake'
)
if ($IncludeLintTools) {
    $packages += @(
        'mingw-w64-ucrt-x86_64-clang-tools-extra',
        'mingw-w64-ucrt-x86_64-cppcheck'
    )
}

Write-Step "Locating MSYS2"
$msys2Root = Find-Msys2Root
if ($null -eq $msys2Root) {
    Install-Msys2ViaWinget
    $msys2Root = Find-Msys2Root
    if ($null -eq $msys2Root) {
        throw "MSYS2 install reported success but bash.exe was not found in any conventional location. Set MSYS2_ROOT and re-run."
    }
    Write-Ok  "MSYS2 installed at $msys2Root"
} else {
    Write-Ok  "MSYS2 found at $msys2Root"
}

Add-Msys2ToSessionPath -Msys2Root $msys2Root

Write-Step "Installing/updating UCRT64 packages: $($packages -join ', ')"
Invoke-Pacman -Msys2Root $msys2Root -Packages $packages
Write-Ok "Packages installed"

Write-Step "Verifying tools resolve from $msys2Root\ucrt64\bin"
foreach ($exe in @('gcc.exe','g++.exe','cmake.exe','ninja.exe','ld.lld.exe')) {
    $expected = Join-Path "$msys2Root\ucrt64\bin" $exe
    if (Test-Path -LiteralPath $expected) {
        Write-Ok "$exe at $expected"
    } else {
        Write-Warn2 "$exe not found at $expected (build may still work if it resolves elsewhere on PATH)"
    }
}

if ($SkipDoctor) {
    Write-Step "Skipping doctor.sh (-SkipDoctor)"
} else {
    $doctor = Join-Path $PSScriptRoot "doctor.sh"
    $bash = Join-Path $msys2Root "usr\bin\bash.exe"
    if (-not (Test-Path -LiteralPath $doctor)) {
        Write-Warn2 "doctor.sh not found at $doctor -- skipped."
    } elseif (-not (Test-Path -LiteralPath $bash)) {
        Write-Warn2 "bash.exe not found at $bash -- doctor.sh skipped."
    } else {
        Write-Step "Running doctor.sh"
        & $bash -lc "'$($doctor -replace '\\','/')'"
        # doctor.sh exit codes: 0=GREEN, 1=RED (required failure), 2=YELLOW (warn-only).
        # GREEN and YELLOW are both build-ready; RED means a required tool is missing
        # or wrong version and the build will fail.
        switch ($LASTEXITCODE) {
            0 { }   # GREEN -- nothing to do
            2 { }   # YELLOW -- optional lint tools missing; build still works
            1 { throw "doctor.sh reported RED (required check failed). See output above for the [FAIL] lines and their install hints." }
            default { throw "doctor.sh returned unexpected exit code $LASTEXITCODE." }
        }
    }
}

Write-Host ""
Write-Ok "Bootstrap complete. To build now in this session:"
Write-Host "    cmake --preset ninja-iter-msys2"
Write-Host "    cmake --build --preset ninja-iter-msys2"
Write-Host ""
Write-Host "For new shells you'll need MSYS2 on PATH. Either:"
Write-Host "  - re-run this script (instant -- it's idempotent), or"
Write-Host "  - prepend `$msys2Root\ucrt64\bin and `$msys2Root\usr\bin to PATH yourself."

# doctor.sh returns 2 on YELLOW (optional tools missing). We treat that as success
# for bootstrap purposes; override the inherited exit code so callers see 0.
exit 0
