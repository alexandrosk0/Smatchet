#!/usr/bin/env bash
# scripts/dev/local/build-deploy-and-open-unreal.sh — one-shot loop: package the
# Win64 DX12 libs, deploy the plugin into an Unreal project, rebuild that
# project's editor target, then open the .uproject.
#
# Bash port of the retired build_deploy_and_open_unreal.ps1.
#
# Usage:
#   bash scripts/dev/local/build-deploy-and-open-unreal.sh
#   bash scripts/dev/local/build-deploy-and-open-unreal.sh --release --force-configure
#   bash scripts/dev/local/build-deploy-and-open-unreal.sh --project-file "D:/MyGame/MyGame.uproject"

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

PROJECT_FILE="${SMATCHET_UNREAL_PROJECT_FILE:-C:/Users/alexk/Documents/Unreal Projects/TestProject/TestProject.uproject}"
BUILD_DIR=""
# Inherited from the environment when set (the contract the retired .ps1 had);
# --unreal-build-bat overrides it.
UNREAL_BUILD_BAT="${UNREAL_BUILD_BAT:-}"
UNREAL_PLATFORM="Win64"
UNREAL_CONFIGURATION="Development"
FORCE_RELEASE=0
FORCE_CONFIGURE=0

usage() {
    cat <<'EOF'
Usage: bash scripts/dev/local/build-deploy-and-open-unreal.sh [options]

  --project-file <path>       .uproject to rebuild + open (env: SMATCHET_UNREAL_PROJECT_FILE)
  --build-dir <dir>           CMake build dir (default: <repo>/build/vs-unreal-msvc)
  --unreal-build-bat <path>   Engine Build.bat (env: UNREAL_BUILD_BAT; else newest UE_* install)
  --unreal-platform <p>       UBT platform (default: Win64)
  --unreal-configuration <c>  UBT configuration (default: Development)
  --release                   Force the packaged libs to Release
  --force-configure           Wipe the CMake build dir and re-configure
  -h, --help                  Show this help
EOF
}

# `shift 2` fails WITHOUT shifting when the option value is missing, which
# would spin this loop forever (no `set -e` here). Reject that up front.
need_value() {
    [ "$1" -ge 2 ] || { echo "build-deploy-and-open-unreal: $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --project-file)         need_value $# "$1"; PROJECT_FILE="$2"; shift 2 ;;
        --build-dir)            need_value $# "$1"; BUILD_DIR="$2"; shift 2 ;;
        --unreal-build-bat)     need_value $# "$1"; UNREAL_BUILD_BAT="$2"; shift 2 ;;
        --unreal-platform)      need_value $# "$1"; UNREAL_PLATFORM="$2"; shift 2 ;;
        --unreal-configuration) need_value $# "$1"; UNREAL_CONFIGURATION="$2"; shift 2 ;;
        --release)              FORCE_RELEASE=1; shift ;;
        --force-configure)      FORCE_CONFIGURE=1; shift ;;
        -h|--help)              usage; exit 0 ;;
        *)
            echo "build-deploy-and-open-unreal: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

die() { echo "build-deploy-and-open-unreal: $*" >&2; exit 1; }

winpath() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    else
        printf '%s' "$1"
    fi
}

[ -f "$PROJECT_FILE" ] || die "Unreal project file not found: $PROJECT_FILE"

PROJECT_DIR="$(cd "$(dirname "$PROJECT_FILE")" && pwd)"
PROJECT_NAME="$(basename "$PROJECT_FILE")"
PROJECT_NAME="${PROJECT_NAME%.uproject}"

# UBT ships far more configurations than the packaging step does. Debug-family
# UBT configs want the Debug libs; everything else links the Release libs.
if [ "$FORCE_RELEASE" -eq 1 ]; then
    PACKAGE_CONFIGURATION="Release"
else
    case "$UNREAL_CONFIGURATION" in
        Debug|DebugGame) PACKAGE_CONFIGURATION="Debug" ;;
        *)               PACKAGE_CONFIGURATION="Release" ;;
    esac
fi

# Build.bat: explicit flag / UNREAL_BUILD_BAT env > newest UE_* install. The
# glob sorts lexically, so UE_5.10 would lose to UE_5.7 — sort -V instead.
resolve_build_bat() {
    local candidate newest=""
    if [ -n "$UNREAL_BUILD_BAT" ]; then
        [ -f "$UNREAL_BUILD_BAT" ] || die "Unreal Build.bat not found: $UNREAL_BUILD_BAT"
        printf '%s' "$UNREAL_BUILD_BAT"
        return 0
    fi
    newest="$(
        for candidate in "C:/Program Files/Epic Games"/UE_*/Engine/Build/BatchFiles/Build.bat; do
            [ -f "$candidate" ] && printf '%s\n' "$candidate"
        done | sort -V | tail -1
    )"
    [ -n "$newest" ] || die "could not locate Unreal Build.bat. Pass --unreal-build-bat or set UNREAL_BUILD_BAT."
    printf '%s' "$newest"
}

BUILD_BAT="$(resolve_build_bat)" || exit 1

echo "==> Unreal project: $PROJECT_FILE"
echo "==> Editor target: ${PROJECT_NAME}Editor $UNREAL_PLATFORM $UNREAL_CONFIGURATION"
echo "==> Packaged library configuration: $PACKAGE_CONFIGURATION"
echo "==> Build.bat: $BUILD_BAT"

PACKAGE_ARGS=(--project-root "$PROJECT_DIR" --configuration "$PACKAGE_CONFIGURATION")
[ -n "$BUILD_DIR" ] && PACKAGE_ARGS+=(--build-dir "$BUILD_DIR")
[ "$FORCE_CONFIGURE" -eq 1 ] && PACKAGE_ARGS+=(--force-configure)

bash "$SCRIPT_DIR/package-unreal-plugin-msvc.sh" "${PACKAGE_ARGS[@]}" \
    || die "packaging/deploy step failed."

echo "==> Rebuilding Unreal editor target..."
cmd.exe /c "$(winpath "$BUILD_BAT")" "${PROJECT_NAME}Editor" "$UNREAL_PLATFORM" "$UNREAL_CONFIGURATION" \
    "$(winpath "$PROJECT_FILE")" -WaitMutex || die "Unreal build failed."

echo "==> Opening $PROJECT_FILE ..."
cmd.exe /c start "" "$(winpath "$PROJECT_FILE")" || die "could not open the .uproject."

echo "==> Done."
