<#
.SYNOPSIS
    Run a command inside the MSVC (vcvars64) developer environment.

.DESCRIPTION
    PowerShell sibling of scripts/dev/with-msvc-env.sh. Subagents doing
    dual-target builds in .claude/worktrees/<id>/ don't get cl.exe on PATH and
    were each re-deriving the vswhere -> vcvars64 dance independently. This is
    the one wrapper to call instead:

        powershell -ExecutionPolicy Bypass -File scripts/dev/with-msvc.ps1 cmake --build --preset ninja-iter-msvc --target SmatchetStandalone

    It discovers a VC-tools VS install via vswhere, imports vcvars64.bat's
    environment into this session (pinning the toolset so a newer side-by-side
    toolset can't load STL headers that break the cached cl.exe — STL1001), then
    executes the passed argument vector in that environment.

.NOTES
    Toolset pin resolution order: $env:SMATCHET_VCVARS_VER, then
    build.msvc_toolset_pin in project.config.json. If neither resolves, vcvars64
    is called unpinned (newest). Exit codes: 2 = vswhere/vcvars not found or no
    VC-tools install; otherwise the wrapped command's own exit code.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]] $Command
)

$ErrorActionPreference = 'Stop'

# Repo root = two levels up from scripts/dev/.
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

# --- Resolve the toolset pin (optional; mirrors with-msvc-env.sh) -------------
$VcvarsVer = $env:SMATCHET_VCVARS_VER
if (-not $VcvarsVer) {
    $cfg = Join-Path $RepoRoot 'project.config.json'
    if (Test-Path $cfg) {
        try {
            $VcvarsVer = (Get-Content $cfg -Raw | ConvertFrom-Json).build.msvc_toolset_pin
        } catch {
            $VcvarsVer = $null  # malformed config: fall through to unpinned
        }
    }
}

# --- Locate a VC-tools VS install via vswhere ---------------------------------
$VsWhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $VsWhere)) {
    Write-Error "with-msvc: vswhere.exe not found at $VsWhere. Install Visual Studio 2017+ (Community is free)."
    exit 2
}

# Deliberately NOT -latest: on a box with a pinned-toolset VS plus a newer
# BuildTools, -latest can pick the BuildTools whose vcvars breaks the cached
# cl.exe. Enumerate every VC-tools install; if a pin is set, prefer the one that
# actually ships VC\Tools\MSVC\<pin>*; else take the first.
$installs = & $VsWhere -products '*' `
    -requires 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' `
    -property installationPath
if (-not $installs) {
    Write-Error 'with-msvc: no VS install with the VC toolchain found (vswhere returned nothing).'
    exit 2
}

$VsInstall = $null
foreach ($cand in @($installs)) {
    if (-not $cand) { continue }
    if ($VcvarsVer -and (Test-Path (Join-Path $cand "VC\Tools\MSVC\$VcvarsVer*"))) {
        $VsInstall = $cand; break
    }
    if (-not $VsInstall) { $VsInstall = $cand }  # first VC-tools install as fallback
}

$Vcvars = Join-Path $VsInstall 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $Vcvars)) {
    Write-Error "with-msvc: vcvars64.bat not found under $VsInstall."
    exit 2
}

# --- Import vcvars64 env, then exec the command -------------------------------
# Run vcvars in a child cmd, dump `set`, and apply each VAR=VALUE to this session.
$verArg = if ($VcvarsVer) { "-vcvars_ver=$VcvarsVer" } else { '' }
$setDump = & cmd.exe /c "call `"$Vcvars`" $verArg >nul 2>&1 && set"
foreach ($line in $setDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2]
    }
}

# Execute the passed argument vector in the now-MSVC environment.
$exe = $Command[0]
$rest = @()
if ($Command.Count -gt 1) { $rest = $Command[1..($Command.Count - 1)] }
& $exe @rest
exit $LASTEXITCODE
