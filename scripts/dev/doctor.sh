#!/usr/bin/env bash
# doctor.sh -- Smatchet toolchain pre-flight check (bash port of doctor.ps1).
#
# Same check list, same exit codes as doctor.ps1:
#   0 -- all required + warn checks pass (Doctor: GREEN)
#   1 -- one or more required checks failed (Doctor: RED)
#   2 -- only warn-only checks failed (Doctor: YELLOW)
#
# Required checks:
#   - cmake >= 3.24
#   - ninja present
#   - git present
#   - MSYS2 UCRT64 gcc (>= 13)  -- "toolchain reachable": `which gcc`
#                                  resolving under /ucrt64/ or $MSYSTEM ==
#                                  UCRT64 on MSYS2.
#   - g++ present
#   - lld / ld.lld present
#   - python >= 3.10
#   - >= 4 GB free under <repo>/build
#
# Warn-only checks:
#   - PATH contains the MSYS2 UCRT64 toolchain dir -- "on PATH at shell
#     time". Required by scripts/dev/ bash wrappers that resolve gcc via
#     $PATH directly. NOT required for `cmake --preset` (presets prepend
#     UCRT64 bin via MSYSTEM_PREFIX). False-failed on JetBrains-bundled-
#     MinGW hosts where a working gcc resolves outside /ucrt64/.
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
# Companion: scripts/dev/doctor.ps1 (PowerShell version on Windows).
# Verification: scripts/dev/test-doctor.sh.

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
    set -- $a
    local a1=$1 a2=$2 a3=$3
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
    write_fail 'ninja' 'install via MSYS2: pacman -S mingw-w64-ucrt-x86_64-ninja (ships with mingw-w64-ucrt-x86_64-toolchain)'
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

# gcc (>= 13) -- "toolchain reachable". A gcc resolving outside /ucrt64/
# (e.g. JetBrains-bundled MinGW) is acceptable because `cmake --preset
# ninja-iter-msys2` prepends the UCRT64 prefix via MSYSTEM_PREFIX before
# invoking the compiler. The "MSYS2 UCRT64 bin on PATH" check is warn-only
# below.
gcc_path=$(command -v gcc 2>/dev/null || true)
gcc_line=$(tool_version gcc --version)
if [ -z "$gcc_line" ]; then
    write_fail 'gcc' 'install: winget install MSYS2.MSYS2 then pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-lld'
else
    gcc_ver=$(parse_version "$gcc_line")
    if [ -z "$gcc_ver" ]; then
        write_fail 'gcc' "could not parse version from: $gcc_line"
    else
        gcc_major=$(echo "$gcc_ver" | awk '{print $1}')
        if [ "$gcc_major" -lt 13 ]; then
            write_fail 'gcc' "found gcc $gcc_ver, need >= 13 -- update: pacman -Syu then pacman -S mingw-w64-ucrt-x86_64-toolchain"
        else
            origin=""
            case "$gcc_path" in
                */ucrt64/*) origin=" (under /ucrt64/)" ;;
                *) origin=" (resolves to $gcc_path)" ;;
            esac
            write_pass 'gcc' "$gcc_line$origin"
        fi
    fi
fi

# g++ present
gxx_line=$(tool_version g++ --version)
if [ -z "$gxx_line" ]; then
    write_fail 'g++' 'install via MSYS2: pacman -S mingw-w64-ucrt-x86_64-toolchain'
else
    write_pass 'g++' "$gxx_line"
fi

# lld / ld.lld present
lld_line=$(tool_version lld --version)
if [ -z "$lld_line" ]; then
    lld_line=$(tool_version ld.lld --version)
fi
if [ -z "$lld_line" ]; then
    write_fail 'lld' 'install via MSYS2: pacman -S mingw-w64-ucrt-x86_64-lld (required by ninja-iter-msys2 linker step)'
else
    write_pass 'lld' "$lld_line"
fi

# python >= 3.10
py_line=$(tool_version python --version)
if [ -z "$py_line" ]; then
    py_line=$(tool_version python3 --version)
fi
if [ -z "$py_line" ]; then
    write_fail 'python' 'install: winget install Python.Python.3.12 (or pacman -S mingw-w64-ucrt-x86_64-python)'
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

# PATH contains MSYS2 UCRT64 toolchain dir -- warn-only because `cmake
# --preset ninja-iter-msys2` prepends the UCRT64 prefix via MSYSTEM_PREFIX
# regardless. This check matters for scripts/dev/ bash wrappers that
# resolve gcc via $PATH directly. False-failed on JetBrains-bundled-MinGW
# hosts pre-split.
path_has_ucrt=0
IFS=':' read -r -a path_parts <<< "$PATH"
for p in "${path_parts[@]}"; do
    case "$p" in
        */ucrt64/bin|/ucrt64/bin|/c/msys64/ucrt64/bin|/C/msys64/ucrt64/bin)
            path_has_ucrt=1; break ;;
    esac
done
# Windows-style PATH entries (semicolons) also valid when running this script
# under Git Bash / pwsh: parse $PATH for "msys64\ucrt64\bin" or "msys64/ucrt64/bin".
if [ "$path_has_ucrt" -eq 0 ]; then
    case "$PATH" in
        *msys64/ucrt64/bin*|*msys64\\ucrt64\\bin*) path_has_ucrt=1 ;;
    esac
fi
if [ "$path_has_ucrt" -eq 0 ]; then
    write_warn 'PATH' 'MSYS2 UCRT64 bin dir (e.g. /ucrt64/bin or C:\msys64\ucrt64\bin) not on PATH -- prepend it for direct-gcc invocation by scripts/dev/ (not required for cmake --preset)'
else
    write_pass 'PATH' 'contains MSYS2 UCRT64 bin dir'
fi

cppcheck_line=$(tool_version cppcheck --version)
if [ -z "$cppcheck_line" ]; then
    write_warn 'cppcheck' 'install via MSYS2: pacman -S mingw-w64-ucrt-x86_64-cppcheck (used by scripts/dev/run_cppcheck.py)'
else
    write_pass 'cppcheck' "$cppcheck_line"
fi

clang_tidy_line=$(tool_version clang-tidy --version)
if [ -z "$clang_tidy_line" ]; then
    write_warn 'clang-tidy' 'install via MSYS2: pacman -S mingw-w64-ucrt-x86_64-clang-tools-extra (used by lint hook)'
else
    write_pass 'clang-tidy' "$clang_tidy_line"
fi

clang_format_line=$(tool_version clang-format --version)
if [ -z "$clang_format_line" ]; then
    write_warn 'clang-format' 'install via MSYS2: pacman -S mingw-w64-ucrt-x86_64-clang-tools-extra (used by lint hook)'
else
    write_pass 'clang-format' "$clang_format_line"
fi

# Opt-in: OpenCppCoverage check is skipped by default. Set the env var
# SMATCHET_DOCTOR_CHECK_COVERAGE=1 to enable. Windows-only; install via
# `choco install opencppcoverage` or the releases page. CI runners install
# it themselves, so this check is only relevant for local coverage runs.
if [ "${SMATCHET_DOCTOR_CHECK_COVERAGE:-0}" = "1" ]; then
    occ_line=$(tool_version OpenCppCoverage --help 2>/dev/null | head -n 1 || true)
    if [ -z "$occ_line" ]; then
        write_warn 'OpenCppCoverage' 'install via Chocolatey (choco install opencppcoverage) or releases page (used by scripts/dev/coverage.sh; CI installs it via Chocolatey, so this is only relevant for local coverage runs)'
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
