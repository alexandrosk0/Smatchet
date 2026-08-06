#!/usr/bin/env bash
# scripts/dev/local/run-standalone.sh — launch an already-built SmatchetStandalone.
#
# Bash port of the retired run_standalone.ps1.
#
# Prints the resolved exe + its mtime before launching, plus a sibling-exe table
# when more than one build dir holds a Smatchet.exe. That table is the cheap
# defence against the classic "I rebuilt but ran yesterday's exe from the other
# preset" confusion.
#
# Usage:
#   bash scripts/dev/local/run-standalone.sh
#   bash scripts/dev/local/run-standalone.sh --preset ninja-debug-msvc
#   bash scripts/dev/local/run-standalone.sh --build-dir build/ninja-debug-msvc
#   bash scripts/dev/local/run-standalone.sh -- --config /tmp/smatchet_config.json

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# shellcheck source=scripts/common/smatchet-cmake-common.sh
. "$REPO_ROOT/scripts/common/smatchet-cmake-common.sh"

PRESET="ninja-debug-msvc"
TARGET="SmatchetStandalone"
EXE_NAME=""
BUILD_DIR=""
CONFIGURATION=""
STANDALONE_ARGS=()

usage() {
    cat <<'EOF'
Usage: bash scripts/dev/local/run-standalone.sh [options] [-- <app args>]

  --preset <name>        CMake preset to resolve the build dir from
  --target <name>        CMake target (default: SmatchetStandalone)
  --exe-name <name>      Override the executable file name
  --build-dir <path>     Use this build dir instead of resolving the preset
  --configuration <cfg>  Multi-config sub-directory (e.g. Debug)
  -h, --help             Show this help

Anything after `--` is forwarded to the app and makes the launch synchronous.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --preset)        PRESET="${2:-}"; shift 2 ;;
        --target)        TARGET="${2:-}"; shift 2 ;;
        --exe-name)      EXE_NAME="${2:-}"; shift 2 ;;
        --build-dir)     BUILD_DIR="${2:-}"; shift 2 ;;
        --configuration) CONFIGURATION="${2:-}"; shift 2 ;;
        -h|--help)       usage; exit 0 ;;
        --)              shift; STANDALONE_ARGS+=("$@"); break ;;
        *)
            echo "run-standalone: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

die() { echo "run-standalone: $*" >&2; exit 1; }

if [ -z "$BUILD_DIR" ]; then
    LOCATION="$(smatchet_resolve_build_location "$REPO_ROOT" "$PRESET")" || exit 1
    BUILD_DIR="$(printf '%s\n' "$LOCATION" | sed -n 's/^BUILD_DIR=//p')"
    # An explicit --configuration wins over the build preset's own.
    if [ -z "$CONFIGURATION" ]; then
        CONFIGURATION="$(printf '%s\n' "$LOCATION" | sed -n 's/^CONFIGURATION=//p')"
    fi
fi

case "$BUILD_DIR" in
    /*|[A-Za-z]:[/\\]*) ;;
    *) BUILD_DIR="$REPO_ROOT/$BUILD_DIR" ;;
esac

if [ -z "$EXE_NAME" ]; then
    # The SmatchetStandalone target carries an OUTPUT_NAME override.
    if [ "$TARGET" = "SmatchetStandalone" ]; then
        EXE_NAME="Smatchet.exe"
    else
        EXE_NAME="$TARGET.exe"
    fi
elif [ "${EXE_NAME##*.}" = "$EXE_NAME" ]; then
    EXE_NAME="$EXE_NAME.exe"
fi

if [ -n "$CONFIGURATION" ]; then
    EXE="$BUILD_DIR/$CONFIGURATION/$EXE_NAME"
else
    EXE="$BUILD_DIR/$EXE_NAME"
fi

[ -f "$EXE" ] || die "expected executable missing: $EXE"

exe_mtime() { date -r "$1" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo "unknown"; }

EXE_ABS="$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")"

echo "  Exe : $EXE_ABS"
echo "  Time: $(exe_mtime "$EXE_ABS")"

# --- Sibling comparison -----------------------------------------------------
SIBLINGS=(
    "$REPO_ROOT/build/ninja-debug-msvc/Smatchet.exe"
    "$REPO_ROOT/build/ninja-iter-msvc/Smatchet.exe"
    "$REPO_ROOT/build/ninja-publish-msvc/Smatchet.exe"
)
SIBLING_FOUND=0
for s in "${SIBLINGS[@]}"; do
    if [ -f "$s" ] && [ "$(cd "$(dirname "$s")" && pwd)/$(basename "$s")" = "$EXE_ABS" ]; then
        SIBLING_FOUND=1
        break
    fi
done
[ "$SIBLING_FOUND" -eq 0 ] && SIBLINGS+=("$EXE_ABS")

ROWS=()
for path in "${SIBLINGS[@]}"; do
    [ -f "$path" ] || continue
    abs="$(cd "$(dirname "$path")" && pwd)/$(basename "$path")"
    marker=""
    [ "$abs" = "$EXE_ABS" ] && marker="<<< selected"
    ROWS+=("  [$(exe_mtime "$abs")]  $abs  $marker")
done

if [ "${#ROWS[@]}" -gt 1 ]; then
    echo ""
    echo "Sibling exe comparison:"
    printf '%s\n' "${ROWS[@]}"
    echo ""
fi

# --- Launch -----------------------------------------------------------------
# With args this is a CLI invocation: run in the foreground so the caller gets
# stdout/stderr and the exit code. With no args it is an interactive GUI launch:
# detach and print the PID so the shell comes straight back.
if [ ${#STANDALONE_ARGS[@]} -gt 0 ]; then
    "$EXE_ABS" "${STANDALONE_ARGS[@]}"
    exit $?
fi

"$EXE_ABS" >/dev/null 2>&1 &
echo "  PID : $!"
