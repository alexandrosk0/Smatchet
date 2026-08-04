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
    build.msvc_toolset_pin in project.config.json. If neither resolves, vcvars
    is called unpinned (newest). Exit codes: 78 = wrapper-level toolchain
    failure (bad SMATCHET_MSVC_ARCH, vswhere missing, no VC-tools install, or
    the vcvars batch missing); otherwise the wrapped command's own exit code,
    propagated verbatim. 78 (sysexits EX_CONFIG) is deliberately outside the
    range cmake/ninja/ctest return, so a caller can tell "no usable toolchain"
    apart from "the wrapped build itself failed with that code" -- a plain
    `exit 2` was ambiguous with a child process that exited 2.

    Target arch: $env:SMATCHET_MSVC_ARCH (default x64) selects the vcvars batch
    (arm64 -> vcvarsamd64_arm64.bat cross from x64, vcvarsarm64.bat native) and
    the VC.Tools component vswhere requires.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]] $Command
)

$ErrorActionPreference = 'Stop'

# Wrapper-level "no usable MSVC toolchain" status. Distinct from any exit code the
# wrapped command can plausibly return (cmake/ninja/ctest use 1/2/8), so build.ps1
# can print install hints for THIS failure without mistaking a child's exit 2 for it.
$ToolchainMissingExit = 78

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

# --- Resolve the target architecture (mirrors with-msvc-env.sh) ---------------
# $env:SMATCHET_MSVC_ARCH (default x64) selects the vcvars batch and the VC.Tools
# component vswhere requires. Host arch from PROCESSOR_ARCHITEW6432 (set under
# emulation/WOW64) falling back to PROCESSOR_ARCHITECTURE: cross from x64 picks
# the amd64_arm64 batch; a native ARM64 host picks the arm64 batch.
$Arch = $env:SMATCHET_MSVC_ARCH
if ([string]::IsNullOrWhiteSpace($Arch)) { $Arch = 'x64' }
$HostArch = $env:PROCESSOR_ARCHITEW6432
if ([string]::IsNullOrWhiteSpace($HostArch)) { $HostArch = $env:PROCESSOR_ARCHITECTURE }
$HostIsArm64 = ($HostArch -and $HostArch -match '^(?i)arm64$')
switch ($Arch.ToLowerInvariant()) {
    'x64' {
        $VcRequires = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
        $VcvarsBat = if ($HostIsArm64) { 'vcvarsarm64_amd64.bat' } else { 'vcvars64.bat' }
    }
    'arm64' {
        $VcRequires = 'Microsoft.VisualStudio.Component.VC.Tools.ARM64'
        $VcvarsBat = if ($HostIsArm64) { 'vcvarsarm64.bat' } else { 'vcvarsamd64_arm64.bat' }
    }
    default {
        # [Console]::Error.WriteLine (not Write-Error): with $ErrorActionPreference
        # = 'Stop', Write-Error is terminating and would exit 1 before the explicit
        # exit runs, breaking the documented exit-code contract.
        [Console]::Error.WriteLine("with-msvc: invalid SMATCHET_MSVC_ARCH='$Arch' (expected x64 or arm64).")
        exit $ToolchainMissingExit
    }
}

# --- Locate a VC-tools VS install via vswhere ---------------------------------
$VsWhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $VsWhere)) {
    [Console]::Error.WriteLine("with-msvc: vswhere.exe not found at $VsWhere. Install Visual Studio 2017+ (Community is free).")
    exit $ToolchainMissingExit
}

# Deliberately NOT -latest: on a box with a pinned-toolset VS plus a newer
# BuildTools, -latest can pick the BuildTools whose vcvars breaks the cached
# cl.exe. Enumerate every VC-tools install; if a pin is set, prefer the one that
# actually ships VC\Tools\MSVC\<pin>*; else take the first.
$installs = & $VsWhere -products '*' `
    -requires $VcRequires `
    -property installationPath
if (-not $installs) {
    [Console]::Error.WriteLine("with-msvc: no VS install with the $VcRequires component found (vswhere returned nothing for arch $Arch).")
    exit $ToolchainMissingExit
}

$VsInstall = $null
foreach ($cand in @($installs)) {
    if (-not $cand) { continue }
    if ($VcvarsVer -and (Test-Path (Join-Path $cand "VC\Tools\MSVC\$VcvarsVer*"))) {
        $VsInstall = $cand; break
    }
    if (-not $VsInstall) { $VsInstall = $cand }  # first VC-tools install as fallback
}

$Vcvars = Join-Path $VsInstall "VC\Auxiliary\Build\$VcvarsBat"
if (-not (Test-Path $Vcvars)) {
    [Console]::Error.WriteLine("with-msvc: $VcvarsBat not found under $VsInstall (arch $Arch; install the matching VC toolset component).")
    exit $ToolchainMissingExit
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

# vcvars64 prepends the VS-bundled LLVM (older clang) to PATH. The ninja-clang-*
# presets call bare `clang-cl`, which would then resolve to that older VS clang
# instead of the standalone LLVM that CI pins (both workflows echo LLVM\bin onto
# the path after msvc-dev-cmd). Mirror CI: put standalone LLVM first. Guarded —
# no-op if absent; harmless for MSVC presets (cl/link.exe aren't in LLVM\bin).
$LlvmBin = 'C:\Program Files\LLVM\bin'
if (Test-Path (Join-Path $LlvmBin 'clang-cl.exe')) {
    $env:PATH = $LlvmBin + ';' + $env:PATH
}

# Execute the passed argument vector in the now-MSVC environment.
$exe = $Command[0]
$rest = @()
if ($Command.Count -gt 1) { $rest = $Command[1..($Command.Count - 1)] }
& $exe @rest
exit $LASTEXITCODE
