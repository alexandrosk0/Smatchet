#!/usr/bin/env bash
# scripts/dev/local/build-msvc-asan.sh — configure + build the MSVC ASan preset.
#
# Bash port of the retired build-msvc-asan.ps1. The PowerShell original carried
# its own copy of the vswhere -> pinned-toolset -> vcvars dance; this one just
# wraps with-msvc-env.sh, which is the single source of truth for that (PATH
# scrub, toolset pin from build.msvc_toolset_pin, SMATCHET_MSVC_ARCH handling).
#
# MSVC ASan is x64-only at the pinned toolset — SMATCHET_MSVC_ARCH=arm64 needs a
# 14.50-era toolset and will likely fail at link until the pin moves.
#
# Usage:
#   bash scripts/dev/local/build-msvc-asan.sh
#   bash scripts/dev/local/build-msvc-asan.sh --preset ninja-msvc-asan

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

PRESET="ninja-msvc-asan"

usage() {
    cat <<'EOF'
Usage: bash scripts/dev/local/build-msvc-asan.sh [options]

  --preset <name>   Sanitizer preset (default: ninja-msvc-asan)
  -h, --help        Show this help
EOF
}

# `shift 2` fails WITHOUT shifting when the option value is missing, which
# would spin this loop forever (no `set -e` here). Reject that up front.
need_value() {
    [ "$1" -ge 2 ] || { echo "build-msvc-asan: $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --preset)  need_value $# "$1"; PRESET="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *)
            echo "build-msvc-asan: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ "${SMATCHET_MSVC_ARCH:-x64}" = "arm64" ]; then
    echo "build-msvc-asan: warning: SMATCHET_MSVC_ARCH=arm64 - MSVC ASan is x64-only at the pinned toolset; link may fail." >&2
fi

# Everything after this point runs inside the imported MSVC environment.
if [ "${SMATCHET_ASAN_INNER:-0}" = "1" ]; then
    cd "$REPO_ROOT" || exit 1

    echo "=== cl.exe version ==="
    cl 2>&1 | head -2 || true

    echo "=== Configuring $PRESET ==="
    cmake --preset "$PRESET" -DSMATCHET_ENABLE_STRICT_WARNINGS=ON || exit $?

    echo "=== Building $PRESET ==="
    cmake --build --preset "$PRESET"
    exit $?
fi

SMATCHET_ASAN_INNER=1 \
    bash "$REPO_ROOT/scripts/dev/with-msvc-env.sh" \
    bash "$SCRIPT_DIR/build-msvc-asan.sh" --preset "$PRESET"
