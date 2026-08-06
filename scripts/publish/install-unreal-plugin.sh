#!/usr/bin/env bash
# scripts/publish/install-unreal-plugin.sh — copy SmatchetImGuiPlugin into an
# Unreal project's Plugins folder or an engine's Marketplace folder.
#
# Bash port of the retired install_unreal_plugin.ps1. Ships inside the packaged
# plugin zip, so it must stand alone next to a sibling SmatchetImGuiPlugin/ dir
# as well as run from the repo.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PLUGIN_ROOT=""
PROJECT_ROOT=""
ENGINE_ROOT=""
FORCE=0

usage() {
    cat <<'EOF'
Usage: bash install-unreal-plugin.sh (--project-root <dir> | --engine-root <dir>) [options]

  --project-root <dir>  Unreal project root (must contain a .uproject)
  --engine-root <dir>   Unreal engine root (must contain Engine/Plugins)
  --plugin-root <dir>   Plugin source (default: sibling SmatchetImGuiPlugin/, else
                        the repo's Source/UnrealPlugins/SmatchetImGuiPlugin)
  --force               Replace an existing destination plugin directory
  -h, --help            Show this help
EOF
}

# `shift 2` fails WITHOUT shifting when the option value is missing; under
# `set -e` that aborts with no explanation. Reject it up front with a message.
need_value() {
    [ "$1" -ge 2 ] || { echo "$(basename "$0"): $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --plugin-root)    need_value $# "$1"; PLUGIN_ROOT="$2"; shift 2 ;;
        --plugin-root=*)  PLUGIN_ROOT="${1#*=}"; shift ;;
        --project-root)   need_value $# "$1"; PROJECT_ROOT="$2"; shift 2 ;;
        --project-root=*) PROJECT_ROOT="${1#*=}"; shift ;;
        --engine-root)    need_value $# "$1"; ENGINE_ROOT="$2"; shift 2 ;;
        --engine-root=*)  ENGINE_ROOT="${1#*=}"; shift ;;
        --force)          FORCE=1; shift ;;
        -h|--help)        usage; exit 0 ;;
        *)
            echo "install-unreal-plugin: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

die() { echo "install-unreal-plugin: $*" >&2; exit 1; }

resolve_existing_path() {
    local value="$1" description="$2"
    [ -n "${value//[[:space:]]/}" ] || die "$description path is required."
    [ -e "$value" ] || die "$description path not found: $value"
    (cd "$value" 2>/dev/null && pwd) || die "$description path not found: $value"
}

if [ -z "${PLUGIN_ROOT//[[:space:]]/}" ]; then
    if [ -d "$SCRIPT_DIR/SmatchetImGuiPlugin" ]; then
        PLUGIN_ROOT="$SCRIPT_DIR/SmatchetImGuiPlugin"
    else
        PLUGIN_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)/Source/UnrealPlugins/SmatchetImGuiPlugin"
    fi
fi

RESOLVED_PLUGIN_ROOT="$(resolve_existing_path "$PLUGIN_ROOT" "Plugin root")"
[ -f "$RESOLVED_PLUGIN_ROOT/SmatchetImGuiPlugin.uplugin" ] \
    || die "Plugin root does not contain SmatchetImGuiPlugin.uplugin: $RESOLVED_PLUGIN_ROOT"

TARGET_COUNT=0
if [ -n "${PROJECT_ROOT//[[:space:]]/}" ]; then
    TARGET_COUNT=$((TARGET_COUNT + 1))
fi
if [ -n "${ENGINE_ROOT//[[:space:]]/}" ]; then
    TARGET_COUNT=$((TARGET_COUNT + 1))
fi
[ "$TARGET_COUNT" -eq 1 ] || die "Specify exactly one target: --project-root or --engine-root."

if [ -n "${PROJECT_ROOT//[[:space:]]/}" ]; then
    RESOLVED_PROJECT_ROOT="$(resolve_existing_path "$PROJECT_ROOT" "Project root")"
    # No pipe: `grep -q` closing early can SIGPIPE `find` into exit 141, which
    # `set -o pipefail` would turn into a bogus "no .uproject" failure.
    if [ -z "$(find "$RESOLVED_PROJECT_ROOT" -maxdepth 1 -type f -name '*.uproject' -print -quit)" ]; then
        die "Project root does not contain a .uproject file: $RESOLVED_PROJECT_ROOT"
    fi
    DEST_ROOT="$RESOLVED_PROJECT_ROOT/Plugins"
else
    RESOLVED_ENGINE_ROOT="$(resolve_existing_path "$ENGINE_ROOT" "Engine root")"
    ENGINE_PLUGINS_ROOT="$RESOLVED_ENGINE_ROOT/Engine/Plugins"
    [ -d "$ENGINE_PLUGINS_ROOT" ] \
        || die "Engine root does not contain Engine/Plugins: $RESOLVED_ENGINE_ROOT"
    DEST_ROOT="$ENGINE_PLUGINS_ROOT/Marketplace"
fi

mkdir -p "$DEST_ROOT"

DEST_PLUGIN_DIR="$DEST_ROOT/SmatchetImGuiPlugin"
if [ -d "$DEST_PLUGIN_DIR" ]; then
    [ "$FORCE" -eq 1 ] \
        || die "Destination already exists: $DEST_PLUGIN_DIR. Re-run with --force to replace it."
    rm -rf "$DEST_PLUGIN_DIR"
fi

cp -R "$RESOLVED_PLUGIN_ROOT" "$DEST_PLUGIN_DIR"
echo "Installed SmatchetImGuiPlugin to: $DEST_PLUGIN_DIR"
