#!/usr/bin/env bash
# scripts/dev/with-msvc-env.sh — source vcvars64 env into a sub-shell, then exec.
#
# Companion to BUILD.md § Common Workflows. PowerShell side has
# build_and_run.ps1 that handles vcvars64 via call-then-inherit; bash side had
# nothing until this script. Without it, `cmake --build --preset
# ninja-iter-msvc` fails from bash with "Cannot open include file: 'stdio.h'"
# because cl.exe lacks INCLUDE / LIB env.
#
# Usage:
#   bash scripts/dev/with-msvc-env.sh cmake --build --preset ninja-iter-msvc --target SmatchetStandalone
#   bash scripts/dev/with-msvc-env.sh cmake --version
#   bash scripts/dev/with-msvc-env.sh cl                          # prints compiler banner
#
# How it works: `vswhere.exe -property installationPath` returns the path of
# the latest installed VS edition (Community / Professional / Enterprise /
# BuildTools) without hard-coding a glob. We then invoke `powershell.exe -c
# 'cmd.exe /c "call vcvars64 && set"'` — PowerShell handles the quote / path
# escaping cleanly where Git Bash's direct `cmd.exe //c` form gets mangled by
# MSYS path conversion. Parse the env dump and re-export VS-relevant vars
# into this bash process.
#
# Exit codes:
#   0 — pass-through from the wrapped command
#   2 — vswhere.exe / vcvars64.bat not found, OR no VS install detected

set -euo pipefail

if [ $# -eq 0 ]; then
    echo "Usage: bash scripts/dev/with-msvc-env.sh <command> [args...]" >&2
    exit 2
fi

# vswhere.exe ships with every VS 2017+ install at a stable path. Use it to
# discover the install path without hard-coding edition / version globs.
VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
if [ ! -x "$VSWHERE" ]; then
    echo "with-msvc-env: vswhere.exe not found at: $VSWHERE" >&2
    echo "  Install Visual Studio 2017 or newer (Community edition is free)." >&2
    exit 2
fi

# `-latest -products *` covers BuildTools + IDE editions. `-requires
# Microsoft.VisualStudio.Component.VC.Tools.x86.x64` filters out installs that
# lack the VC toolchain (e.g. a VS instance with only the "ASP.NET and web
# development" workload) — without the filter, vswhere happily returns such
# an install and the resulting `vcvars64.bat` lookup fails further downstream
# with a confusing "file not found" instead of the clear early-exit here.
# -property installationPath returns the absolute install dir (e.g.
# "C:\Program Files\Microsoft Visual Studio\2022\Community").
VS_INSTALL="$("$VSWHERE" -latest -products '*' \
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
    -property installationPath 2>/dev/null | tr -d '\r')"
if [ -z "$VS_INSTALL" ]; then
    echo "with-msvc-env: no Visual Studio install with VC tools detected by vswhere.exe" >&2
    echo "  (install requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64)" >&2
    exit 2
fi

VCVARS_WIN="$VS_INSTALL\\VC\\Auxiliary\\Build\\vcvars64.bat"
# Bash-side existence check — convert C:\... to /c/... for `[ -f ]`.
VCVARS_BASH="${VCVARS_WIN//\\//}"
case "$VCVARS_BASH" in
    [A-Za-z]:/*)
        # Lowercase the drive letter (`/c/...`) since Git Bash mounts use lowercase.
        DRIVE="$(printf '%s' "${VCVARS_BASH:0:1}" | tr 'A-Z' 'a-z')"
        VCVARS_BASH="/$DRIVE${VCVARS_BASH:2}"
        ;;
esac
if [ ! -f "$VCVARS_BASH" ]; then
    echo "with-msvc-env: vcvars64.bat not found at: $VCVARS_WIN" >&2
    exit 2
fi

# Wrap the cmd.exe vcvars64 call in PowerShell. PowerShell handles the
# quote / path escaping cleanly. Direct `cmd.exe //c "..."` from Git Bash is
# mangled by MSYS path-conversion in non-obvious ways (the `//` becomes `/`
# and embedded backslashes in paths can collide with bash escape rules).
# Tested on Git Bash 2.x + VS 2022 / 2025 + PowerShell 5.1 + pwsh 7.x.
PS_CMD="cmd.exe /c 'call \"$VCVARS_WIN\" >nul 2>&1 && set'"

# Import VS-relevant vars into this bash process. The while loop avoids awk
# regex on backslash-heavy paths.
while IFS='=' read -r key val; do
    case "$key" in
        # MSVC compiler env
        INCLUDE|LIB|LIBPATH|VCINSTALLDIR|VCToolsInstallDir|VCToolsRedistDir|VCIDEInstallDir)
            export "$key=$val" ;;
        # Windows SDK
        WindowsSdkDir|WindowsSDKLibVersion|WindowsSDKVersion|WindowsLibPath|UCRTVersion|UniversalCRTSdkDir)
            export "$key=$val" ;;
        # PATH gets cl.exe, link.exe, the Windows SDK bins, VS-bundled cmake +
        # ninja, etc. cmd.exe's `set` emits Windows-shape PATH (`;`-separated,
        # `C:\...` paths). Bash needs Unix-shape (`:`-separated, `/c/...`).
        # cygpath -p does the round-trip. The Windows-shape Path is also
        # exported so any tool that reads `Path` (rare but exists) sees the
        # native form.
        Path|PATH)
            export Path="$val"
            export PATH="$(cygpath -p "$val" 2>/dev/null || echo "$val")"
            ;;
    esac
done < <(powershell.exe -NoProfile -Command "$PS_CMD" 2>/dev/null | tr -d '\r')

# Verify cl.exe is reachable — gives a clear diagnostic when the import
# silently failed (e.g. vcvars64 wrote to a different shell context).
if ! command -v cl >/dev/null 2>&1; then
    echo "with-msvc-env: cl.exe not on PATH after vcvars64 import; environment may be partial" >&2
    exit 2
fi

exec "$@"
