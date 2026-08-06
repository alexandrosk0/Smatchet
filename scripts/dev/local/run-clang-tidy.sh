#!/usr/bin/env bash
# scripts/dev/local/run-clang-tidy.sh — clang-tidy over first-party C++.
#
# Bash port of the retired run_clang_tidy.ps1. Runs over every .cpp under
# Source/Core/ and Source/Plugins/ against a build tree that exported
# compile_commands.json (CMAKE_EXPORT_COMPILE_COMMANDS=ON).
#
# Prerequisites:
#   - clang-tidy on PATH
#   - a configured build: cmake --preset ninja-debug-msvc
#
# Usage:
#   bash scripts/dev/local/run-clang-tidy.sh
#   bash scripts/dev/local/run-clang-tidy.sh --build-dir build/ninja-debug-msvc
#   bash scripts/dev/local/run-clang-tidy.sh --checks "-*,clang-analyzer-deadcode.*"
#   bash scripts/dev/local/run-clang-tidy.sh --fix

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

BUILD_DIR=""
CLANG_TIDY="clang-tidy"
CHECKS=""
FIX=0
QUIET=0

usage() {
    cat <<'EOF'
Usage: bash scripts/dev/local/run-clang-tidy.sh [options]

  --build-dir <path>   Build tree holding compile_commands.json
                       (default: build/ninja-debug-msvc)
  --clang-tidy <bin>   clang-tidy executable (default: clang-tidy)
  --checks <list>      Value for -checks=
  --fix                Pass -fix to clang-tidy
  --quiet              Pass --quiet to clang-tidy
  -h, --help           Show this help
EOF
}

# `shift 2` fails WITHOUT shifting when the option value is missing, which
# would spin this loop forever (no `set -e` here). Reject that up front.
need_value() {
    [ "$1" -ge 2 ] || { echo "run-clang-tidy: $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir)  need_value $# "$1"; BUILD_DIR="$2"; shift 2 ;;
        --clang-tidy) need_value $# "$1"; CLANG_TIDY="$2"; shift 2 ;;
        --checks)     need_value $# "$1"; CHECKS="$2"; shift 2 ;;
        --fix)        FIX=1; shift ;;
        --quiet)      QUIET=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *)
            echo "run-clang-tidy: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

die() { echo "run-clang-tidy: $*" >&2; exit 1; }

if [ -z "$BUILD_DIR" ]; then
    BUILD_DIR="$REPO_ROOT/build/ninja-debug-msvc"
else
    case "$BUILD_DIR" in
        /*|[A-Za-z]:[/\\]*) ;;
        *) BUILD_DIR="$REPO_ROOT/$BUILD_DIR" ;;
    esac
fi

DB="$BUILD_DIR/compile_commands.json"
[ -f "$DB" ] || die "missing compile_commands.json at '$DB'. From the repo root run: cmake --preset ninja-debug-msvc"

command -v "$CLANG_TIDY" >/dev/null 2>&1 || die "$CLANG_TIDY is not on PATH (install LLVM)."

CORE_ROOT="$REPO_ROOT/Source/Core"
PLUGINS_ROOT="$REPO_ROOT/Source/Plugins"
[ -d "$CORE_ROOT" ]    || die "expected directory: $CORE_ROOT"
[ -d "$PLUGINS_ROOT" ] || die "expected directory: $PLUGINS_ROOT"

SOURCES=()
while IFS= read -r src; do
    SOURCES+=("$src")
done < <(find "$CORE_ROOT" "$PLUGINS_ROOT" -type f -name '*.cpp' | sort -u)

[ "${#SOURCES[@]}" -gt 0 ] || die "no .cpp files found under Source/Core or Source/Plugins."

TIDY_ARGS=(-p "$BUILD_DIR")
[ -n "$CHECKS" ]    && TIDY_ARGS+=("-checks=$CHECKS")
[ "$FIX" -eq 1 ]    && TIDY_ARGS+=(-fix)
[ "$QUIET" -eq 1 ]  && TIDY_ARGS+=(--quiet)

echo "clang-tidy: ${#SOURCES[@]} file(s), build dir: $BUILD_DIR"

# Keep going after a failing file and report the last non-zero code, so one bad
# translation unit does not hide findings in the rest of the tree.
LAST_RC=0
for src in "${SOURCES[@]}"; do
    echo ""
    echo "$src"
    "$CLANG_TIDY" "${TIDY_ARGS[@]}" "$src" || LAST_RC=$?
done

exit "$LAST_RC"
