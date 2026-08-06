#!/usr/bin/env bash
# scripts/dev/local/build-and-deploy-unreal-plugin.sh — build the Win64 DX12
# artifacts and deploy the plugin tree into an Unreal project.
#
# Bash port of the retired build_and_deploy_unreal_plugin.ps1.
#
# This is a thin convenience wrapper over package-unreal-plugin-msvc.sh — that
# script owns the generator detection, the stale-cache resets and the deploy
# copy. Keeping one implementation avoids the two drifting apart (they already
# had, before the port: the wrapper hardcoded "Visual Studio 17 2022").
#
# Usage:
#   bash scripts/dev/local/build-and-deploy-unreal-plugin.sh
#   bash scripts/dev/local/build-and-deploy-unreal-plugin.sh --release
#   bash scripts/dev/local/build-and-deploy-unreal-plugin.sh --project-root "D:/MyGame"

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

PROJECT_ROOT=""
BUILD_DIR=""
CONFIGURATION="Debug"
FORCE_CONFIGURE=0

usage() {
    cat <<'EOF'
Usage: bash scripts/dev/local/build-and-deploy-unreal-plugin.sh [options]

  --project-root <dir>   Unreal project to deploy into (env: SMATCHET_UNREAL_PROJECT_ROOT)
  --build-dir <dir>      CMake build dir (default: <repo>/build/vs-unreal-msvc)
  --release              Package Release instead of Debug
  --force-configure      Wipe the build dir and re-configure
  -h, --help             Show this help
EOF
}

# `shift 2` fails WITHOUT shifting when the option value is missing, which
# would spin this loop forever (no `set -e` here). Reject that up front.
need_value() {
    [ "$1" -ge 2 ] || { echo "build-and-deploy-unreal-plugin: $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --project-root)    need_value $# "$1"; PROJECT_ROOT="$2"; shift 2 ;;
        --build-dir)       need_value $# "$1"; BUILD_DIR="$2"; shift 2 ;;
        --release)         CONFIGURATION="Release"; shift ;;
        --force-configure) FORCE_CONFIGURE=1; shift ;;
        -h|--help)         usage; exit 0 ;;
        *)
            echo "build-and-deploy-unreal-plugin: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

ARGS=(--configuration "$CONFIGURATION")
[ -n "$PROJECT_ROOT" ] && ARGS+=(--project-root "$PROJECT_ROOT")
[ -n "$BUILD_DIR" ] && ARGS+=(--build-dir "$BUILD_DIR")
[ "$FORCE_CONFIGURE" -eq 1 ] && ARGS+=(--force-configure)

exec bash "$SCRIPT_DIR/package-unreal-plugin-msvc.sh" "${ARGS[@]}"
