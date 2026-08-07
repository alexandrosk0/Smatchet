#!/usr/bin/env bash
# scripts/dev/local/rebuild-testproject-plugin.sh — deploy the Smatchet plugin
# into an Unreal project and rebuild that project's editor target, resolving the
# engine install from the project's own EngineAssociation.
#
# Bash port of the retired rebuild_testproject_plugin.ps1.
#
# Usage:
#   bash scripts/dev/local/rebuild-testproject-plugin.sh
#   bash scripts/dev/local/rebuild-testproject-plugin.sh --release
#   bash scripts/dev/local/rebuild-testproject-plugin.sh --uproject "D:/MyGame/MyGame.uproject"

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$REPO_ROOT/scripts/common/smatchet-cmake-common.sh"

UPROJECT_PATH="${SMATCHET_UNREAL_PROJECT_FILE:-C:/Users/alexk/Documents/Unreal Projects/TestProject/TestProject.uproject}"
ENGINE_ROOT=""
RELEASE=0

usage() {
    cat <<'EOF'
Usage: bash scripts/dev/local/rebuild-testproject-plugin.sh [options]

  --uproject <path>     .uproject to deploy into + rebuild (env: SMATCHET_UNREAL_PROJECT_FILE)
  --engine-root <dir>   Engine install root (default: resolved from EngineAssociation)
  --release             Deploy the Release libs instead of Debug
  -h, --help            Show this help
EOF
}

# `shift 2` fails WITHOUT shifting when the option value is missing, which
# would spin this loop forever (no `set -e` here). Reject that up front.
need_value() {
    [ "$1" -ge 2 ] || { echo "rebuild-testproject-plugin: $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --uproject)    need_value $# "$1"; UPROJECT_PATH="$2"; shift 2 ;;
        --engine-root) need_value $# "$1"; ENGINE_ROOT="$2"; shift 2 ;;
        --release)     RELEASE=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        *)
            echo "rebuild-testproject-plugin: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

die() { echo "rebuild-testproject-plugin: $*" >&2; exit 1; }

# shellcheck source=scripts/common/unreal-batch.sh
. "$SCRIPT_DIR/../../common/unreal-batch.sh" || die "could not source unreal-batch.sh"

winpath() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    else
        printf '%s' "$1"
    fi
}

[ -f "$UPROJECT_PATH" ] || die "Unreal project file not found: $UPROJECT_PATH"

PROJECT_DIR="$(cd "$(dirname "$UPROJECT_PATH")" && pwd)"
PROJECT_NAME="$(basename "$UPROJECT_PATH")"
PROJECT_NAME="${PROJECT_NAME%.uproject}"

read_engine_association() {
    local py
    py="$(smatchet_python)" || return 1
    "$py" - "$1" <<'PY' | tr -d '\r'
import json, sys
try:
    with open(sys.argv[1], "r", encoding="utf-8-sig") as fh:
        print(json.load(fh).get("EngineAssociation", ""))
except (OSError, ValueError):
    pass
PY
}

# Launcher installs register their real install dir under the association GUID /
# version key; only the "UE_<version>" default layout is guessable. Probe the
# registry first, both the 64-bit view and the WOW6432Node twin.
resolve_engine_root() {
    local association="$1" key value
    [ -n "$association" ] || return 1
    for key in \
        "HKLM\\SOFTWARE\\EpicGames\\Unreal Engine\\$association" \
        "HKLM\\SOFTWARE\\WOW6432Node\\EpicGames\\Unreal Engine\\$association"; do
        value="$(reg.exe query "$key" /v InstalledDirectory 2>/dev/null \
            | sed -n 's/.*InstalledDirectory[[:space:]]*REG_SZ[[:space:]]*//p' | head -1 | tr -d '\r')"
        if [ -n "$value" ] && [ -d "$value" ]; then
            printf '%s' "$value"
            return 0
        fi
    done
    value="C:/Program Files/Epic Games/UE_$association"
    if [ -d "$value" ]; then
        printf '%s' "$value"
        return 0
    fi
    return 1
}

if [ -z "$ENGINE_ROOT" ]; then
    ASSOCIATION="$(read_engine_association "$UPROJECT_PATH")"
    [ -n "$ASSOCIATION" ] || die "no EngineAssociation in $UPROJECT_PATH — pass --engine-root."
    ENGINE_ROOT="$(resolve_engine_root "$ASSOCIATION")" \
        || die "could not resolve the engine install for EngineAssociation '$ASSOCIATION' — pass --engine-root."
fi

[ -d "$ENGINE_ROOT" ] || die "engine root not found: $ENGINE_ROOT"

BUILD_BAT="$ENGINE_ROOT/Engine/Build/BatchFiles/Build.bat"
[ -f "$BUILD_BAT" ] || die "Build.bat not found under the engine root: $BUILD_BAT"

echo "==> Unreal project: $UPROJECT_PATH"
echo "==> Engine root: $ENGINE_ROOT"

DEPLOY_ARGS=(--project-root "$PROJECT_DIR")
[ "$RELEASE" -eq 1 ] && DEPLOY_ARGS+=(--release)

bash "$SCRIPT_DIR/build-and-deploy-unreal-plugin.sh" "${DEPLOY_ARGS[@]}" \
    || die "plugin deploy step failed."

echo "==> Rebuilding ${PROJECT_NAME} (Win64 Development)..."
run_unreal_batch "$(winpath "$BUILD_BAT")" "$PROJECT_NAME" Win64 Development \
    "-Project=$(winpath "$UPROJECT_PATH")" -WaitMutex -NoHotReloadFromIDE \
    || die "Unreal build failed."

echo "==> Done."
