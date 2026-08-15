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
#   SMATCHET_MSVC_ARCH=arm64 bash scripts/dev/with-msvc-env.sh cmake --build --preset ninja-publish-msvc-arm64
#
# Target arch: $SMATCHET_MSVC_ARCH selects the vcvars batch (default x64 →
# vcvars64.bat; arm64 → vcvarsamd64_arm64.bat cross from an x64 host, or
# vcvarsarm64.bat native on an ARM64 host) and the VC.Tools component vswhere
# requires. cl.exe's target ISA follows from the sourced batch.
#
# How it works: `vswhere.exe -property installationPath` enumerates installed
# VS editions (Community / Professional / Enterprise / BuildTools) without
# hard-coding a glob; we pick the first that ships the pinned MSVC toolset
# (build.msvc_toolset_pin in project.config.json, or $SMATCHET_VCVARS_VER) and
# call the arch-appropriate vcvars batch with `-vcvars_ver=<pin>` so a newer side-by-side toolset can't
# shadow it (the STL1001 failure mode on multi-VS boxes). We then invoke
# `powershell.exe -c 'cmd.exe /c "call vcvars64 ... && set"'` — PowerShell
# handles the quote / path escaping cleanly where Git Bash's direct `cmd.exe
# //c` form gets mangled by MSYS path conversion. Parse the env dump and
# re-export VS-relevant vars into this bash process.
#
# Exit codes:
#   0 — pass-through from the wrapped command
#   2 — vswhere.exe / vcvars64.bat not found, OR no VS install detected
#
# $SMATCHET_MSVC_ENV_FAIL_RC overrides that 2. A caller that also propagates the
# wrapped command's exit code (build.sh) cannot otherwise tell "the MSVC env
# could not be set up" apart from "the build itself exited 2", and would report a
# missing toolchain for an ordinary build failure.
#
# $SMATCHET_MSVC_STATUS_FILE (optional) names a path this script writes a
# `toolchain-missing` sentinel to on each of its OWN failure exits — an
# out-of-band status channel. The exit code alone is structurally ambiguous no
# matter which number it carries: the tail of this script is `exec "$@"`, so
# every code except the setup-failure one belongs to the wrapped command, and a
# wrapped command that happens to return the chosen failure code would be
# misread. Callers should key "the toolchain is missing" on the sentinel's
# presence and treat the exit code as pass-through only.

set -euo pipefail

_FAIL_RC="${SMATCHET_MSVC_ENV_FAIL_RC:-2}"
# A zero (or non-numeric) override would report an MSVC-setup failure as success
# without ever running the wrapped command. Only a real failing status is valid.
if ! printf '%s' "$_FAIL_RC" | grep -qE '^[1-9][0-9]*$' || [ "$_FAIL_RC" -gt 255 ]; then
    echo "with-msvc-env: SMATCHET_MSVC_ENV_FAIL_RC must be a decimal exit code in 1-255: $_FAIL_RC" >&2
    exit 2
fi

# Every own-failure exit goes through here so the sentinel can never drift out
# of step with the exit code. The wrapped command runs via `exec` below, so
# nothing it does can reach this function — sentinel presence is proof the
# failure was ours.
fail_env() {
    if [ -n "${SMATCHET_MSVC_STATUS_FILE:-}" ]; then
        printf 'toolchain-missing\n' > "$SMATCHET_MSVC_STATUS_FILE" 2>/dev/null || true
    fi
    exit "$_FAIL_RC"
}

if [ $# -eq 0 ]; then
    echo "Usage: bash scripts/dev/with-msvc-env.sh <command> [args...]" >&2
    fail_env
fi

# Resolve the MSVC toolset to pin. On a multi-VS box (e.g. VS2022 Community
# carrying 14.38 side-by-side with a newer VS BuildTools carrying only 14.50),
# an unpinned vcvars loads the newest STL headers against the cached older
# cl.exe -> error STL1001. Precedence: $SMATCHET_VCVARS_VER (explicit override)
# > build.msvc_toolset_pin in project.config.json. project-config.sh resolves
# the repo root from its own location, so this works from any CWD; the source
# is non-fatal so the env var alone can still drive selection if the config is
# unavailable.
_self="${BASH_SOURCE[0]:-$0}"
_self_dir="$(cd "$(dirname "$_self")" && pwd)"
# shellcheck source=scripts/dev/project-config.sh
. "$_self_dir/project-config.sh" >/dev/null 2>&1 || true
VCVARS_VER="${SMATCHET_VCVARS_VER:-${PC_BUILD_MSVC_TOOLSET_PIN:-}}"
# Python-free fallback: a build must not hard-fail just because project-config.sh's
# python is missing/broken (e.g. a Windows Store python stub). Grep the scalar
# straight out of project.config.json.
if [ -z "$VCVARS_VER" ]; then
    _cfg="$_self_dir/../../project.config.json"
    if [ -f "$_cfg" ]; then
        # `|| true`: under `set -euo pipefail`, a no-match grep returns non-zero,
        # which (via pipefail) would propagate out of the command-substitution and
        # kill the script BEFORE the empty-check fallback below. Swallow it so an
        # absent/!matched key falls through to the clear error at line 59.
        VCVARS_VER="$(grep -oE '"msvc_toolset_pin"[[:space:]]*:[[:space:]]*"[0-9]+\.[0-9]+"' "$_cfg" | grep -oE '[0-9]+\.[0-9]+' | head -1 || true)"
    fi
fi
if [ -z "$VCVARS_VER" ]; then
    echo "with-msvc-env: no MSVC toolset version resolved." >&2
    echo "  Set \$SMATCHET_VCVARS_VER or build.msvc_toolset_pin in project.config.json." >&2
    fail_env
fi

# Target architecture (x64 default; arm64 for the Windows-on-ARM port). The arch
# decides BOTH which vcvars batch we source (so cl.exe targets the right ISA) and
# which VC.Tools component vswhere must require. Host arch comes from
# PROCESSOR_ARCHITEW6432 (set when an emulated/32-bit process runs on a 64-bit
# host) falling back to PROCESSOR_ARCHITECTURE: a cross build from an x64 host
# picks the amd64_arm64 cross batch; a native ARM64 host picks the arm64 batch.
# Normalise the requested arch to lowercase so ARM64/Arm64/X64 all resolve (the
# PowerShell wrappers ToLower() theirs; keep parity).
SMATCHET_MSVC_ARCH="$(printf '%s' "${SMATCHET_MSVC_ARCH:-x64}" | tr 'A-Z' 'a-z')"
case "${PROCESSOR_ARCHITEW6432:-${PROCESSOR_ARCHITECTURE:-AMD64}}" in
    [Aa][Rr][Mm]64) _host_arch="arm64" ;;
    *)              _host_arch="x64" ;;
esac
case "$SMATCHET_MSVC_ARCH" in
    x64)
        VCVARS_REQUIRES="Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
        if [ "$_host_arch" = "arm64" ]; then VCVARS_BAT="vcvarsarm64_amd64.bat"; else VCVARS_BAT="vcvars64.bat"; fi
        ;;
    arm64)
        VCVARS_REQUIRES="Microsoft.VisualStudio.Component.VC.Tools.ARM64"
        if [ "$_host_arch" = "arm64" ]; then VCVARS_BAT="vcvarsarm64.bat"; else VCVARS_BAT="vcvarsamd64_arm64.bat"; fi
        ;;
    *)
        echo "with-msvc-env: invalid SMATCHET_MSVC_ARCH='$SMATCHET_MSVC_ARCH' (expected x64 or arm64)." >&2
        fail_env
        ;;
esac

# vswhere.exe ships with every VS 2017+ install at a stable path. Use it to
# discover the install path without hard-coding edition / version globs.
VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
if [ ! -x "$VSWHERE" ]; then
    echo "with-msvc-env: vswhere.exe not found at: $VSWHERE" >&2
    echo "  Install Visual Studio 2017 or newer (Community edition is free)." >&2
    fail_env
fi

# `-products *` covers BuildTools + IDE editions. `-requires <VCVARS_REQUIRES>`
# (the arch-selected VC.Tools component — x86.x64 for x64, ARM64 for the arm64
# cross/native toolset) filters out installs that lack the needed VC toolchain
# (e.g. a VS instance with only the "ASP.NET and web development" workload, or an
# x64-only install when arm64 was requested). We deliberately do NOT pass
# `-latest`: on a box with
# VS2022 Community (carrying the pinned 14.38 toolset) plus a newer VS
# BuildTools (14.50 only), `-latest` returns the BuildTools install, whose
# vcvars loads 14.50 STL headers and breaks the cached 14.38 cl.exe (STL1001).
# Instead, enumerate every VC-tools install and pick the first that actually
# ships the pinned toolset under VC\Tools\MSVC\<ver>* (the on-disk dir is the
# full 14.38.xxxxx; vcvars accepts the 2-component 14.38 form).
# -property installationPath returns the absolute install dir (e.g.
# "C:\Program Files\Microsoft Visual Studio\2022\Community").
VS_INSTALL=""
while IFS= read -r _cand; do
    if [ -z "$_cand" ]; then continue; fi
    # Convert the Windows install path to a bash path for the [ -d ] glob test.
    _cand_bash="${_cand//\\//}"
    case "$_cand_bash" in
        [A-Za-z]:/*)
            _d="$(printf '%s' "${_cand_bash:0:1}" | tr 'A-Z' 'a-z')"
            _cand_bash="/$_d${_cand_bash:2}"
            ;;
    esac
    for _td in "$_cand_bash"/VC/Tools/MSVC/"$VCVARS_VER"*; do
        if [ -d "$_td" ]; then
            VS_INSTALL="$_cand"
            break
        fi
    done
    if [ -n "$VS_INSTALL" ]; then break; fi
done < <("$VSWHERE" -products '*' \
    -requires "$VCVARS_REQUIRES" \
    -property installationPath 2>/dev/null | tr -d '\r')

if [ -z "$VS_INSTALL" ]; then
    echo "with-msvc-env: no Visual Studio install ships MSVC toolset $VCVARS_VER for arch $SMATCHET_MSVC_ARCH" >&2
    echo "  Installs with the required VC tools ($VCVARS_REQUIRES):" >&2
    "$VSWHERE" -products '*' \
        -requires "$VCVARS_REQUIRES" \
        -property installationPath 2>/dev/null | tr -d '\r' | sed 's/^/    /' >&2
    echo "  Install MSVC v$VCVARS_VER build tools (with the ARM64 component for arm64), or set \$SMATCHET_VCVARS_VER to an installed toolset." >&2
    fail_env
fi

VCVARS_WIN="$VS_INSTALL\\VC\\Auxiliary\\Build\\$VCVARS_BAT"
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
    echo "with-msvc-env: $VCVARS_BAT not found at: $VCVARS_WIN" >&2
    echo "  (arch $SMATCHET_MSVC_ARCH on $_host_arch host — install the matching VC toolset component.)" >&2
    fail_env
fi

# Announce the resolved selection (stderr — never pollutes stdout pass-through).
echo "with-msvc-env: $VS_INSTALL (MSVC toolset $VCVARS_VER, arch $SMATCHET_MSVC_ARCH via $VCVARS_BAT)" >&2

# Wrap the cmd.exe vcvars64 call in PowerShell. PowerShell handles the
# quote / path escaping cleanly. Direct `cmd.exe //c "..."` from Git Bash is
# mangled by MSYS path-conversion in non-obvious ways (the `//` becomes `/`
# and embedded backslashes in paths can collide with bash escape rules).
# Tested on Git Bash 2.x + VS 2022 / 2025 + PowerShell 5.1 + pwsh 7.x.
PS_CMD="cmd.exe /c 'call \"$VCVARS_WIN\" -vcvars_ver=$VCVARS_VER >nul 2>&1 && set'"

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
            command -v cygpath >/dev/null 2>&1 || { echo "with-msvc-env: cygpath required (ships with Git for Windows / MSYS2)" >&2; fail_env; }
            converted=$(cygpath -p "$val" 2>/dev/null || printf '%s' "$val")
            export PATH="$converted"
            ;;
    esac
done < <(powershell.exe -NoProfile -Command "$PS_CMD" 2>/dev/null | tr -d '\r')

# vcvars64 prepends the VS-bundled LLVM (older clang) to PATH. The ninja-clang-*
# presets call bare `clang-cl`, which would then resolve to that older VS clang
# instead of the standalone LLVM that CI pins (both workflows echo LLVM\bin onto
# the path after msvc-dev-cmd). Mirror CI: put standalone LLVM first. Guarded —
# no-op if absent; harmless for MSVC presets (cl/link.exe aren't in LLVM\bin).
_llvm_bin="/c/Program Files/LLVM/bin"
if [ -x "$_llvm_bin/clang-cl.exe" ]; then
    export PATH="$_llvm_bin:$PATH"
fi

# Restore Git's helper dir to PATH + export GIT_EXEC_PATH. vcvars64's `set` dump
# (imported above) REPLACES the bash PATH with the Windows-shape vcvars PATH,
# which drops Git-for-Windows' `libexec/git-core`. That dir holds `git-submodule`
# and the `git-sh-setup` helper it sources. When FetchContent populates a
# submodule-bearing dep (cpr's transitive curl, sol2's Catch, SQLiteCpp's
# googletest, …), cmake spawns `cmd.exe → git submodule update --init`; with
# git-core off PATH that wrapper dies "git-sh-setup: file not found" →
# "Failed to update submodules" → the cold configure of a fresh worktree aborts.
# (tooling.md P2: fresh worktrees cannot cold-configure ninja-ui-test-msvc.)
# Prepending `git --exec-path` makes the helper resolvable AND exporting
# GIT_EXEC_PATH covers any tool that reads the env var directly. Guarded — no-op
# if git is absent (a build that needs git would already have failed the clone).
if command -v git >/dev/null 2>&1; then
    _git_exec_path="$(git --exec-path 2>/dev/null || true)"
    if [ -n "$_git_exec_path" ] && [ -d "$_git_exec_path" ]; then
        export GIT_EXEC_PATH="$_git_exec_path"
        export PATH="$_git_exec_path:$PATH"
    fi
fi

# Verify cl.exe is reachable — gives a clear diagnostic when the import
# silently failed (e.g. vcvars64 wrote to a different shell context).
if ! command -v cl >/dev/null 2>&1; then
    echo "with-msvc-env: cl.exe not on PATH after vcvars64 import; environment may be partial" >&2
    fail_env
fi

exec "$@"
