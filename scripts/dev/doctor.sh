#!/usr/bin/env bash
# doctor.sh -- Smatchet toolchain pre-flight check.
#
# Exit codes:
#   0 -- all required + warn checks pass (Doctor: GREEN)
#   1 -- one or more required checks failed (Doctor: RED)
#   2 -- only warn-only checks failed (Doctor: YELLOW)
#
# Required checks:
#   - cmake >= 3.24
#   - ninja present
#   - git present
#   - cl.exe OR clang-cl reachable (MSVC or Clang/LLVM compiler)
#   - link.exe reachable (MSVC linker — needed for both cl.exe and clang-cl)
#   - python >= 3.10
#   - >= 4 GB free under <repo>/build
#
# Warn-only checks:
#   - cppcheck
#   - clang-tidy
#   - clang-format
#
# Opt-in warn-only checks (gated by env var, skipped by default):
#   - OpenCppCoverage (SMATCHET_DOCTOR_CHECK_COVERAGE=1) -- Windows-only
#     local coverage tool; install via `choco install opencppcoverage`
#     or download from github.com/OpenCppCoverage/OpenCppCoverage/releases.
#     CI runners install it themselves; only enable this check if you're
#     working on coverage gates / threshold tuning locally.
#
# Verification: scripts/dev/test-doctor.sh.
# Windows callers (PowerShell): invoke via `bash scripts/dev/doctor.sh`
# from Git Bash or `wsl bash scripts/dev/doctor.sh`.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

FAIL_COUNT=0
WARN_COUNT=0

color_red()    { printf '\033[31m%s\033[0m\n' "$*"; }
color_green()  { printf '\033[32m%s\033[0m\n' "$*"; }
color_yellow() { printf '\033[33m%s\033[0m\n' "$*"; }

write_pass() { color_green  "[PASS] $1 -- $2"; }
write_fail() { color_red    "[FAIL] $1 -- $2"; FAIL_COUNT=$((FAIL_COUNT + 1)); }
write_warn() { color_yellow "[WARN] $1 -- $2"; WARN_COUNT=$((WARN_COUNT + 1)); }

# Print the first line of `<cmd> --version` (or alt args), or empty if missing.
tool_version() {
    local cmd="$1"; shift
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo ""
        return
    fi
    local out
    out=$("$cmd" "$@" 2>&1 | head -n 1 || true)
    echo "$out"
}

# Parse "X.Y[.Z]" from a line; print "X Y Z" or empty.
parse_version() {
    local text="$1"
    local trio
    trio=$(echo "$text" | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -n 1 || true)
    if [ -z "$trio" ]; then echo ""; return; fi
    echo "$trio" | awk -F. '{ printf "%s %s %s", $1, ($2==""?0:$2), ($3==""?0:$3) }'
}

# Compare two "X Y Z" tuples: returns 0 if $1 >= $2, else 1.
ver_ge() {
    local a="$1" b="$2"
    # Intentional word-splitting: $a / $b are space-separated "X Y Z" tuples.
    # shellcheck disable=SC2086
    set -- $a
    local a1=$1 a2=$2 a3=$3
    # shellcheck disable=SC2086
    set -- $b
    local b1=$1 b2=$2 b3=$3
    if [ "$a1" -ne "$b1" ]; then [ "$a1" -gt "$b1" ] && return 0 || return 1; fi
    if [ "$a2" -ne "$b2" ]; then [ "$a2" -gt "$b2" ] && return 0 || return 1; fi
    if [ "$a3" -ge "$b3" ]; then return 0; else return 1; fi
}

echo "Smatchet doctor -- repo: $REPO_ROOT"
echo ""

# ---------------------------------------------------------------------------
# Required checks
# ---------------------------------------------------------------------------

# cmake >= 3.24
cmake_line=$(tool_version cmake --version)
if [ -z "$cmake_line" ]; then
    write_fail 'cmake' 'install: winget install Kitware.CMake (or your distro package)'
else
    cmake_ver=$(parse_version "$cmake_line")
    if [ -z "$cmake_ver" ]; then
        write_fail 'cmake' "could not parse version from: $cmake_line"
    elif ver_ge "$cmake_ver" "3 24 0"; then
        write_pass 'cmake' "$cmake_line"
    else
        write_fail 'cmake' "found $cmake_ver, need >= 3.24 -- upgrade: winget install Kitware.CMake"
    fi
fi

# ninja present
ninja_line=$(tool_version ninja --version)
if [ -z "$ninja_line" ]; then
    write_fail 'ninja' 'install: included with Visual Studio, or winget install Ninja-build.Ninja'
else
    write_pass 'ninja' "ninja $ninja_line"
fi

# git present
git_line=$(tool_version git --version)
if [ -z "$git_line" ]; then
    write_fail 'git' 'install: winget install Git.Git (or your distro package)'
else
    write_pass 'git' "$git_line"
fi

# C++ compiler: cl.exe (MSVC) or clang-cl (Clang/LLVM). At least one required.
# cl.exe needs a VS Developer Command Prompt; clang-cl needs LLVM on PATH.
cl_found=0
clangcl_found=0
cl_line=$(tool_version cl 2>&1 | head -n 1 || true)
if command -v cl.exe >/dev/null 2>&1; then
    cl_found=1
    cl_ver=$(cl.exe 2>&1 | head -n 1 || true)
    write_pass 'cl.exe' "$cl_ver"
fi
if command -v clang-cl >/dev/null 2>&1; then
    clangcl_found=1
    clangcl_ver=$(clang-cl --version 2>&1 | head -n 1 || true)
    write_pass 'clang-cl' "$clangcl_ver"
fi
if [ "$cl_found" -eq 0 ] && [ "$clangcl_found" -eq 0 ]; then
    write_fail 'compiler' 'need cl.exe (VS Developer Command Prompt) or clang-cl (winget install LLVM.LLVM)'
fi

# MSVC linker (link.exe) -- required for both cl.exe and clang-cl on Windows.
if command -v link.exe >/dev/null 2>&1; then
    # Distinguish MSVC link.exe from coreutils link (which exists on some systems).
    link_out=$(link.exe 2>&1 | head -n 1 || true)
    case "$link_out" in
        *Microsoft*|*LINK*|*Linker*)
            write_pass 'link.exe' "$link_out"
            ;;
        *)
            write_warn 'link.exe' "found link.exe but it may not be MSVC linker: $link_out"
            ;;
    esac
else
    write_warn 'link.exe' 'not found -- run from a VS Developer Command Prompt for MSVC builds'
fi

# python >= 3.10
py_line=$(tool_version python --version)
if [ -z "$py_line" ]; then
    py_line=$(tool_version python3 --version)
fi
if [ -z "$py_line" ]; then
    write_fail 'python' 'install: winget install Python.Python.3.12'
else
    py_ver=$(parse_version "$py_line")
    if [ -z "$py_ver" ]; then
        write_fail 'python' "could not parse version from: $py_line"
    elif ver_ge "$py_ver" "3 10 0"; then
        write_pass 'python' "$py_line"
    else
        write_fail 'python' "found $py_ver, need >= 3.10 -- upgrade: winget install Python.Python.3.12"
    fi
fi

# Disk: >= 4 GB free under <repo>/build
build_dir="$REPO_ROOT/build"
disk_target="$build_dir"
if [ ! -d "$build_dir" ]; then
    disk_target="$REPO_ROOT"
fi
free_kb=$(df -Pk "$disk_target" 2>/dev/null | awk 'NR==2 {print $4}')
if [ -z "$free_kb" ]; then
    write_fail 'disk' "could not check free space at $disk_target"
else
    free_gb=$(awk -v kb="$free_kb" 'BEGIN { printf "%.1f", kb / 1024 / 1024 }')
    free_gb_int=$(awk -v kb="$free_kb" 'BEGIN { printf "%d", kb / 1024 / 1024 }')
    if [ "$free_gb_int" -lt 4 ]; then
        write_fail 'disk' "only ${free_gb} GB free at $disk_target -- need >= 4 GB for FetchContent _deps + LTO objects"
    else
        write_pass 'disk' "${free_gb} GB free at $disk_target"
    fi
fi

# ---------------------------------------------------------------------------
# Warn-only checks
# ---------------------------------------------------------------------------

cppcheck_line=$(tool_version cppcheck --version)
if [ -z "$cppcheck_line" ]; then
    write_warn 'cppcheck' 'install via LLVM or Visual Studio individual components (used by scripts/dev/local/run_cppcheck.py)'
else
    write_pass 'cppcheck' "$cppcheck_line"
fi

clang_tidy_line=$(tool_version clang-tidy --version)
if [ -z "$clang_tidy_line" ]; then
    write_warn 'clang-tidy' 'install via LLVM (winget install LLVM.LLVM) or Visual Studio individual components (used by lint hook)'
else
    write_pass 'clang-tidy' "$clang_tidy_line"
fi

clang_format_line=$(tool_version clang-format --version)
if [ -z "$clang_format_line" ]; then
    write_warn 'clang-format' 'install via LLVM (winget install LLVM.LLVM) or Visual Studio individual components (used by lint hook)'
else
    write_pass 'clang-format' "$clang_format_line"
fi

# Harness provisioned — the session guards (HEAD-drift etc.) live in the
# gitignored .claude/ and are wired only by setup-harness.sh, so a fresh clone
# runs with every guard silently inert (the #913 bootstrap hole). Surface that
# state in the standard preflight instead of relying on a hand-run probe.
harness_probe="$REPO_ROOT/agents/scripts/core/check-harness-provisioned.sh"
if [ -f "$harness_probe" ]; then
    if bash "$harness_probe" --quiet "$REPO_ROOT" >/dev/null 2>&1; then
        write_pass 'harness' 'session guards provisioned (.claude/hooks wired)'
    else
        write_warn 'harness' 'NOT provisioned -- session guards inert; fix: bash agents/scripts/core/setup-harness.sh claude-code'
    fi
fi

# Opt-in: OpenCppCoverage check is skipped by default. Set the env var
# SMATCHET_DOCTOR_CHECK_COVERAGE=1 to enable. Windows-only; install via
# `choco install opencppcoverage` or the releases page. CI runners install
# it themselves, so this check is only relevant for local coverage runs.
if [ "${SMATCHET_DOCTOR_CHECK_COVERAGE:-0}" = "1" ]; then
    occ_line=$(tool_version OpenCppCoverage --help 2>/dev/null | head -n 1 || true)
    if [ -z "$occ_line" ]; then
        write_warn 'OpenCppCoverage' 'install via Chocolatey (choco install opencppcoverage) or releases page (used by scripts/dev/coverage.sh)'
    else
        write_pass 'OpenCppCoverage' "$occ_line"
    fi
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo ""
if [ "$FAIL_COUNT" -gt 0 ]; then
    color_red    "Doctor: RED ($FAIL_COUNT required failure(s), $WARN_COUNT warning(s))"
    exit 1
elif [ "$WARN_COUNT" -gt 0 ]; then
    color_yellow "Doctor: YELLOW ($WARN_COUNT warning(s) -- build works, optional tools missing)"
    exit 2
else
    color_green  "Doctor: GREEN (all checks passed)"
    exit 0
fi
