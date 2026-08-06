#!/usr/bin/env bash
# scripts/dev/local/build-and-run.sh — build a target, then launch it.
#
# Bash port of the retired build_and_run.ps1. Also absorbs the three one-line
# preset shims it used to have siblings for:
#
#   build_and_run_ninja_debug.ps1 -> --preset ninja-debug-msvc
#   build_and_run_vs_debug.ps1    -> --preset ninja-debug-msvc
#   build_and_run_vs_release.ps1  -> --preset ninja-publish-msvc
#
# Usage:
#   bash scripts/dev/local/build-and-run.sh
#   bash scripts/dev/local/build-and-run.sh --preset ninja-debug-msvc --build-only
#   bash scripts/dev/local/build-and-run.sh -- --version

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

PRESET="ninja-iter-msvc"
TARGET="SmatchetStandalone"
BUILD_ONLY=0
RUN_ONLY=0
VERIFY=0
STANDALONE_ARGS=()

usage() {
    cat <<'EOF'
Usage: bash scripts/dev/local/build-and-run.sh [options] [-- <app args>]

  --preset <name>   CMake preset (default: ninja-iter-msvc)
  --target <name>   CMake target (default: SmatchetStandalone)
  --build-only      Compile only; do not launch the app
  --run-only        Launch the existing exe; do not compile
  --verify          After building, run the comment-noise + delta-lint gates
  -h, --help        Show this help

Anything after `--` is forwarded to the standalone app.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --preset)     PRESET="${2:-}"; shift 2 ;;
        --target)     TARGET="${2:-}"; shift 2 ;;
        --build-only) BUILD_ONLY=1; shift ;;
        --run-only)   RUN_ONLY=1; shift ;;
        --verify)     VERIFY=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        --)           shift; STANDALONE_ARGS+=("$@"); break ;;
        *)
            echo "build-and-run: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ "$BUILD_ONLY" -eq 1 ] && [ "$RUN_ONLY" -eq 1 ]; then
    echo "build-and-run: --build-only and --run-only are mutually exclusive." >&2
    exit 2
fi
if [ "$VERIFY" -eq 1 ] && [ "$RUN_ONLY" -eq 1 ]; then
    echo "build-and-run: --verify needs a build; it cannot combine with --run-only." >&2
    exit 2
fi

if [ "$RUN_ONLY" -eq 0 ]; then
    bash "$SCRIPT_DIR/build-standalone.sh" --preset "$PRESET" --target "$TARGET" || exit $?

    if [ "$VERIFY" -eq 1 ]; then
        # --skip-build: we just built. verify.sh only adds the delta-lint gates here.
        bash "$REPO_ROOT/scripts/dev/verify.sh" --skip-build || exit $?
    fi
fi

if [ "$BUILD_ONLY" -eq 1 ]; then
    exit 0
fi

RUN=("bash" "$SCRIPT_DIR/run-standalone.sh" --preset "$PRESET" --target "$TARGET")
if [ ${#STANDALONE_ARGS[@]} -gt 0 ]; then
    RUN+=(-- "${STANDALONE_ARGS[@]}")
fi
"${RUN[@]}"
