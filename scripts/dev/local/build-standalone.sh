#!/usr/bin/env bash
# scripts/dev/local/build-standalone.sh — configure (if needed) + build one target.
#
# Bash port of the retired build_standalone.ps1.
#
# The pre-flight ordering matters and is asserted by test-build-wrapper.sh:
#   1. cmake present
#   2. retired *-msys2 preset          -> migration hint
#   3. ninja* preset                   -> ninja present
#   4. msvc/clang preset without cl    -> name build.sh / with-msvc-env.sh
#   5. clang preset without clang-cl   -> name LLVM
# Every one of these fires BEFORE `cmake --preset`, so a missing toolchain
# produces a one-line diagnosis instead of an opaque CMake compiler-probe error.
#
# Usage:
#   bash scripts/dev/local/build-standalone.sh
#   bash scripts/dev/local/build-standalone.sh --preset ninja-iter-msvc --clean-first

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# shellcheck source=scripts/common/smatchet-cmake-common.sh
. "$REPO_ROOT/scripts/common/smatchet-cmake-common.sh"

PRESET="ninja-debug-msvc"
TARGET="SmatchetStandalone"
FORCE_CONFIGURE=0
CLEAN_FIRST=0

usage() {
    cat <<'EOF'
Usage: bash scripts/dev/local/build-standalone.sh [options]

  --preset <name>     CMake preset (default: ninja-debug-msvc)
  --target <name>     CMake target (default: SmatchetStandalone)
  --force-configure   Re-run `cmake --preset` even when CMakeCache.txt exists
  --clean-first       Pass --clean-first to `cmake --build`
  -h, --help          Show this help
EOF
}

# `shift 2` fails WITHOUT shifting when the option value is missing, which
# would spin this loop forever (no `set -e` here). Reject that up front.
need_value() {
    [ "$1" -ge 2 ] || { echo "build-standalone: $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --preset)          need_value $# "$1"; PRESET="$2"; shift 2 ;;
        --target)          need_value $# "$1"; TARGET="$2"; shift 2 ;;
        --force-configure) FORCE_CONFIGURE=1; shift ;;
        --clean-first)     CLEAN_FIRST=1; shift ;;
        -h|--help)         usage; exit 0 ;;
        *)
            echo "build-standalone: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

die() { echo "build-standalone: $*" >&2; exit 1; }

# 1. cmake
command -v cmake >/dev/null 2>&1 || die "cmake is not on PATH. Install CMake 3.21+ and reopen the shell."

# 2. Retired MSYS2 presets. Checked before the ninja check so the migration hint
#    wins over a generic "ninja missing" message.
case "$PRESET" in
    *-msys2)
        die "preset '$PRESET' is retired. Use ninja-iter-msvc for MSVC or ninja-iter-clang for clang-cl (see docs/agent-rules/build.md)."
        ;;
esac

# 3. Ninja generator presets need ninja.
case "$PRESET" in
    ninja*)
        command -v ninja >/dev/null 2>&1 || die "ninja is not on PATH but preset '$PRESET' uses the Ninja generator. Install Ninja (winget install Ninja-build.Ninja)."
        ;;
esac

# 4. Every msvc/clang preset compiles against the MSVC headers + libs, so the
#    MSVC environment must be live. Substring match on purpose: the token sits
#    mid-name in ninja-msvc-asan / ninja-clang-asan / *-msvc-arm64.
case "$PRESET" in
    *msvc*|*clang*)
        if ! command -v cl >/dev/null 2>&1; then
            die "cl.exe is not on PATH but preset '$PRESET' needs the MSVC environment. Run ./build.sh (it imports the environment), or wrap the command in scripts/dev/with-msvc-env.sh."
        fi
        ;;
esac

# 5. clang presets pin CMAKE_CXX_COMPILER to clang-cl, which only exists when
#    LLVM (or the VS "C++ Clang tools for Windows" component) is installed.
case "$PRESET" in
    *clang*)
        if ! command -v clang-cl >/dev/null 2>&1; then
            die "clang-cl.exe is not on PATH but preset '$PRESET' requires it. Install LLVM (winget install LLVM.LLVM) or the VS 'C++ Clang tools for Windows' component."
        fi
        ;;
esac

# --- Configure (skipped when the cache already exists) ----------------------
BINARY_DIR="$(smatchet_configure_preset_binary_dir "$REPO_ROOT" "$PRESET")" || exit 1

if [ "$FORCE_CONFIGURE" -eq 1 ] || [ ! -f "$BINARY_DIR/CMakeCache.txt" ]; then
    echo "build-standalone: cmake --preset $PRESET"
    (cd "$REPO_ROOT" && cmake --preset "$PRESET") || die "cmake configure failed (preset $PRESET)."
else
    echo "build-standalone: reusing existing configure in $BINARY_DIR (--force-configure to redo)"
fi

# --- Build ------------------------------------------------------------------
BUILD_CMD=(cmake --build --preset "$PRESET" --target "$TARGET")
[ "$CLEAN_FIRST" -eq 1 ] && BUILD_CMD+=(--clean-first)

echo "build-standalone: ${BUILD_CMD[*]}"
(cd "$REPO_ROOT" && "${BUILD_CMD[@]}") || die "build failed (preset $PRESET, target $TARGET)."
