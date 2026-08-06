#!/usr/bin/env bash
# build.sh — one-command build entry point.
#
# Picks a preset when you don't name one, imports the MSVC environment when the
# shell doesn't already have it, then hands off to
# scripts/dev/local/build-and-run.sh.
#
# Bash port of the retired build.ps1.
#
# Usage:
#   ./build.sh                                    # auto-detect preset, build + run
#   ./build.sh --build-only
#   ./build.sh --preset ninja-debug-msvc --build-only
#   ./build.sh --verify                           # build, then the delta-lint gates
#   ./build.sh -- --version                       # args after -- go to the app
#
# Exit codes: pass-through from the delegate; 78 = no usable MSVC toolchain.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

PRESET=""
TARGET="SmatchetStandalone"
BUILD_ONLY=0
RUN_ONLY=0
VERIFY=0
STANDALONE_ARGS=()

usage() {
    cat <<'EOF'
Usage: ./build.sh [options] [-- <app args>]

  --preset <name>   CMake preset (default: auto-detected from the compiler on PATH)
  --target <name>   CMake target (default: SmatchetStandalone)
  --build-only      Compile only; do not launch the app
  --run-only        Launch the existing exe; do not compile
  --verify          After building, run the comment-noise + delta-lint gates
  -h, --help        Show this help

Anything after `--` is forwarded to the standalone app.
EOF
}

# `shift 2` fails WITHOUT shifting when the option value is missing, which
# would spin this loop forever (no `set -e` here). Reject that up front.
need_value() {
    [ "$1" -ge 2 ] || { echo "build.sh: $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --preset)     need_value $# "$1"; PRESET="$2"; shift 2 ;;
        --target)     need_value $# "$1"; TARGET="$2"; shift 2 ;;
        --build-only) BUILD_ONLY=1; shift ;;
        --run-only)   RUN_ONLY=1; shift ;;
        --verify)     VERIFY=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        --)           shift; STANDALONE_ARGS+=("$@"); break ;;
        *)
            echo "build.sh: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

# --- Preset auto-detect ----------------------------------------------------
# cl.exe on PATH means an MSVC dev shell is already active -> the MSVC preset.
# Only clang-cl means an LLVM install with no MSVC env -> the clang preset.
# Neither means we will import the MSVC env below, so the MSVC preset again.
HAVE_CL=0
HAVE_CLANG_CL=0
command -v cl >/dev/null 2>&1 && HAVE_CL=1
command -v clang-cl >/dev/null 2>&1 && HAVE_CLANG_CL=1

if [ -n "$PRESET" ]; then
    echo "build.sh: preset $PRESET (explicit --preset overrides auto-detect)"
else
    if [ "$HAVE_CL" -eq 0 ] && [ "$HAVE_CLANG_CL" -eq 1 ]; then
        PRESET="ninja-iter-clang"
    else
        PRESET="ninja-iter-msvc"
    fi
    echo "build.sh: preset $PRESET (auto-detected)"
fi

# --- Assemble the delegate call --------------------------------------------
DELEGATE=("bash" "$REPO_ROOT/scripts/dev/local/build-and-run.sh" \
    --preset "$PRESET" --target "$TARGET")
[ "$BUILD_ONLY" -eq 1 ] && DELEGATE+=(--build-only)
[ "$RUN_ONLY"   -eq 1 ] && DELEGATE+=(--run-only)
[ "$VERIFY"     -eq 1 ] && DELEGATE+=(--verify)
if [ ${#STANDALONE_ARGS[@]} -gt 0 ]; then
    DELEGATE+=(-- "${STANDALONE_ARGS[@]}")
fi

# --run-only compiles nothing, so it must not require a toolchain at all: gating
# it behind with-msvc-env.sh would refuse to launch an already-built exe on a
# machine with no Visual Studio.
if [ "$HAVE_CL" -eq 1 ] || [ "$RUN_ONLY" -eq 1 ]; then
    if [ "$HAVE_CL" -eq 1 ]; then
        echo "build.sh: cl.exe already on PATH - invoking build-and-run.sh directly"
    else
        echo "build.sh: --run-only needs no compiler - invoking build-and-run.sh directly"
    fi
    "${DELEGATE[@]}"
    rc=$?
else
    echo "build.sh: no cl.exe on PATH - importing the MSVC environment via with-msvc-env.sh"
    # with-msvc-env.sh exits 2 for "could not set up the MSVC environment", which
    # is indistinguishable from a wrapped build that legitimately exited 2. Ask it
    # for a distinct status so the install hints below fire only for a genuinely
    # missing toolchain.
    SMATCHET_MSVC_ENV_FAIL_RC=78 \
        bash "$REPO_ROOT/scripts/dev/with-msvc-env.sh" "${DELEGATE[@]}"
    rc=$?
fi

if [ "$rc" -eq 78 ]; then
    echo "" >&2
    echo "build.sh: no usable MSVC toolchain found." >&2
    echo "  Install Visual Studio Build Tools with the C++ workload:" >&2
    echo '    winget install Microsoft.VisualStudio.2022.BuildTools --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"' >&2
    case "$PRESET" in
        *clang*)
            # clang-cl still compiles against the MSVC headers/libs, so LLVM is an
            # addition to Build Tools, never a substitute for it.
            echo "  This preset also needs clang-cl, so additionally install LLVM:" >&2
            echo "    winget install LLVM.LLVM" >&2
            ;;
    esac
fi

exit "$rc"
