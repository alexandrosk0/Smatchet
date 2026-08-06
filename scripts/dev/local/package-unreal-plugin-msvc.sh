#!/usr/bin/env bash
# scripts/dev/local/package-unreal-plugin-msvc.sh — build the Win64 DX12 artifacts
# for Unreal packaging with the Visual Studio generator, then (unless
# --package-only) deploy the whole plugin tree into an Unreal project.
#
# Bash port of the retired package_unreal_plugin_msvc.ps1.
#
# The MSVC/link.exe-compatible generator is deliberate: Unreal Build Tool links
# the packaged Win64 third-party libs with MSVC. MinGW/MSYS2 archives are not
# ABI-compatible here even when renamed to .lib.
#
# Usage:
#   bash scripts/dev/local/package-unreal-plugin-msvc.sh
#   bash scripts/dev/local/package-unreal-plugin-msvc.sh --package-only
#   bash scripts/dev/local/package-unreal-plugin-msvc.sh --project-root "D:/MyGame"
#   bash scripts/dev/local/package-unreal-plugin-msvc.sh --toolset-version 14.44.35207
#   bash scripts/dev/local/package-unreal-plugin-msvc.sh --vs-generator "Visual Studio 18 2026"

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

PLUGIN_NAME="SmatchetImGuiPlugin"
SOURCE_PLUGIN_DIR="$REPO_ROOT/Source/UnrealPlugins/$PLUGIN_NAME"
FETCHCONTENT_BASE_DIR="$REPO_ROOT/.fetchcontent-msvc"

PROJECT_ROOT="${SMATCHET_UNREAL_PROJECT_ROOT:-C:/Users/alexk/Documents/Unreal Projects/TestProject}"
CONFIGURATION="Debug"
PACKAGE_ONLY=0
CONFIGURE_PRESET="vs-unreal-msvc"
BUILD_DIR=""
FORCE_CONFIGURE=0
VS_GENERATOR=""
TOOLSET_VERSION=""

usage() {
    cat <<'EOF'
Usage: bash scripts/dev/local/package-unreal-plugin-msvc.sh [options]

  --project-root <dir>      Unreal project to deploy into (env: SMATCHET_UNREAL_PROJECT_ROOT)
  --configuration <c>       Release | Debug (default: Debug)
  --package-only            Package the libs; do not copy the plugin anywhere
  --configure-preset <p>    Configure preset (default + only valid: vs-unreal-msvc)
  --build-dir <dir>         CMake build dir (default: <repo>/build/vs-unreal-msvc)
  --force-configure         Wipe the build dir and re-configure
  --vs-generator <name>     Override generator auto-detect (e.g. "Visual Studio 18 2026")
  --toolset-version <v>     Pin the MSVC toolset via -T version=<v> (e.g. 14.44.35207)
  -h, --help                Show this help
EOF
}

# `shift 2` fails WITHOUT shifting when the option value is missing, which
# would spin this loop forever (no `set -e` here). Reject that up front.
need_value() {
    [ "$1" -ge 2 ] || { echo "package-unreal-plugin-msvc: $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --project-root)     need_value $# "$1"; PROJECT_ROOT="$2"; shift 2 ;;
        --configuration)    need_value $# "$1"; CONFIGURATION="$2"; shift 2 ;;
        --configure-preset) need_value $# "$1"; CONFIGURE_PRESET="$2"; shift 2 ;;
        --build-dir)        need_value $# "$1"; BUILD_DIR="$2"; shift 2 ;;
        --vs-generator)     need_value $# "$1"; VS_GENERATOR="$2"; shift 2 ;;
        --toolset-version)  need_value $# "$1"; TOOLSET_VERSION="$2"; shift 2 ;;
        --package-only)     PACKAGE_ONLY=1; shift ;;
        --force-configure)  FORCE_CONFIGURE=1; shift ;;
        -h|--help)          usage; exit 0 ;;
        *)
            echo "package-unreal-plugin-msvc: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

die() { echo "package-unreal-plugin-msvc: $*" >&2; exit 1; }

case "$CONFIGURATION" in
    Release|Debug) ;;
    *) die "--configuration must be Release or Debug (got '$CONFIGURATION')." ;;
esac

# cmake is a native Windows binary; hand it native paths so an MSYS-style
# /c/... never reaches the cache.
winpath() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf '%s' "$1"
    fi
}

[ -d "$SOURCE_PLUGIN_DIR" ] || die "source plugin directory not found: $SOURCE_PLUGIN_DIR"

if [ "$CONFIGURE_PRESET" != "vs-unreal-msvc" ]; then
    die "configure preset '$CONFIGURE_PRESET' is not valid for Win64 Unreal packaging. Unreal Build Tool links with MSVC/link.exe, so use 'vs-unreal-msvc'."
fi

if [ -z "$BUILD_DIR" ]; then
    BUILD_DIR="$REPO_ROOT/build/vs-unreal-msvc"
elif [ "${BUILD_DIR#/}" = "$BUILD_DIR" ] && [ "${BUILD_DIR#?:}" = "$BUILD_DIR" ]; then
    BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
fi

command -v cmake >/dev/null 2>&1 || die "cmake not found on PATH. Install CMake or use a shell where the Visual Studio CMake generator is available."

echo "==> Repo root: $REPO_ROOT"
echo "==> CMake configure preset: $CONFIGURE_PRESET"
echo "==> CMake build directory: $BUILD_DIR"
echo "==> FetchContent base dir: $FETCHCONTENT_BASE_DIR"
echo "==> Requested configuration hint: $CONFIGURATION"

CMAKE_CACHE="$BUILD_DIR/CMakeCache.txt"

# Refuse to rm -rf anything that is not under the repo root.
assert_inside_repo() {
    # Case-folded: Windows paths are case-insensitive, so a --build-dir typed as
    # c:/dev/... names the same folder as a C:/Dev/... repo root and must not be
    # rejected as "outside the repo".
    local target root
    target="$(winpath "$1" | tr '[:upper:]' '[:lower:]')"
    root="$(winpath "$REPO_ROOT" | tr '[:upper:]' '[:lower:]')"
    case "$target" in
        "$root"/*) return 0 ;;
    esac
    die "refusing to remove a directory outside the repo root: $1"
}

reset_build_dir() {
    assert_inside_repo "$1"
    if [ -d "$1" ]; then
        echo "==> Removing stale CMake build directory: $1"
        rm -rf "$1"
    fi
}

cache_value() {
    # cache_value <cache-file> <NAME> -> value, or empty when absent.
    [ -f "$1" ] || return 0
    sed -n "s/^$2:[^=]*=//p" "$1" | head -1 | tr -d '\r'
}

# FetchContent_MakeAvailable drives a per-dependency "<dep>-subbuild" CMake
# project that caches its own CMAKE_GENERATOR. When the top-level generator
# changes (e.g. VS 2022 -> VS 2026 after a toolchain upgrade), resetting the
# main build dir is not enough: the stale subbuild caches still pin the old
# generator and the populate step fails with "Does not match the generator used
# previously". Drop the per-dependency *-build / *-subbuild dirs (keep *-src to
# avoid a full re-download).
reset_stale_fetchcontent() {
    local base_dir="$1" expected="$2" sub cached stale=0
    [ -d "$base_dir" ] || return 0
    assert_inside_repo "$base_dir"

    for sub in "$base_dir"/*-subbuild; do
        [ -d "$sub" ] || continue
        cached="$(cache_value "$sub/CMakeCache.txt" "CMAKE_GENERATOR")"
        if [ -n "$cached" ] && [ "$cached" != "$expected" ]; then
            stale=1
            break
        fi
    done
    [ "$stale" -eq 1 ] || return 0

    echo "==> Stale FetchContent generator caches detected; clearing *-build / *-subbuild under: $base_dir"
    for sub in "$base_dir"/*-build "$base_dir"/*-subbuild; do
        [ -d "$sub" ] && rm -rf "$sub"
    done
    return 0
}

# Detect the installed Visual Studio version and pick the matching CMake
# generator. Hardcoding "Visual Studio 17 2022" broke on machines upgraded to
# VS 2026 (v18): the v17 generator reports "could not find any instance of
# Visual Studio". But blindly taking the newest install (vswhere -latest) is
# also wrong: Unreal links the packaged Win64 libs with its OWN MSVC toolchain
# (VS 2022 / 14.3x-14.4x for UE 5.7 — VS 2026 / 14.50 is not yet whitelisted).
# Building with a newer toolset emits MSVC STL "satellite" symbols (__std_rotate,
# __std_find_first_not_of_trivial_pos_1, ...) absent in Unreal's older link-time
# runtime -> LNK2001/LNK2019.
#
# So: PREFER a VS 2022 instance when one is installed; fall back to the newest VS
# only if no VS 2022 exists. Override with --vs-generator once Unreal ships
# support for a newer toolchain, and pin the exact toolset with --toolset-version.
resolve_vs_generator() {
    local fallback="Visual Studio 17 2022"
    local vswhere="C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    local version major year

    [ -n "$VS_GENERATOR" ] && { printf '%s' "$VS_GENERATOR"; return 0; }
    [ -x "$vswhere" ] || { printf '%s' "$fallback"; return 0; }

    version="$("$vswhere" -version "[17.0,18.0)" -products '*' \
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
        -property installationVersion 2>/dev/null | head -1 | tr -d '\r')"
    [ -n "$version" ] && { printf '%s' "$fallback"; return 0; }

    version="$("$vswhere" -latest -products '*' \
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
        -property installationVersion 2>/dev/null | head -1 | tr -d '\r')"
    [ -n "$version" ] || { printf '%s' "$fallback"; return 0; }

    major="${version%%.*}"
    case "$major" in
        17) year="2022" ;;
        18) year="2026" ;;
        *)
            echo "==> WARN: VS major '$major' (v$version) not in generator map; falling back to '$fallback'." >&2
            printf '%s' "$fallback"
            return 0
            ;;
    esac
    echo "==> WARN: no VS 2022 instance found; using newest 'Visual Studio $major $year'. If the Unreal link fails with unresolved __std_* symbols, install the VS 2022 build tools or pass --toolset-version to match Unreal." >&2
    printf '%s' "Visual Studio $major $year"
}

EXPECTED_GENERATOR="$(resolve_vs_generator)"

# The Unreal light profile: Lua on, MCP/AI/Whisper off. A cache carrying any
# other value is stale and forces a reconfigure.
EXPECTED_FEATURES="SMATCHET_WITH_LUA_AUTOMATION=ON
SMATCHET_WITH_MCP=OFF
SMATCHET_WITH_AI=OFF
SMATCHET_WITH_WHISPER=OFF
SMATCHET_WHISPER_LOCAL_BACKEND=OFF"

GENERATOR_MISMATCH=0
FEATURE_MISMATCH=0
if [ -f "$CMAKE_CACHE" ]; then
    CACHED_GENERATOR="$(cache_value "$CMAKE_CACHE" "CMAKE_GENERATOR")"
    if [ -n "$CACHED_GENERATOR" ] && [ "$CACHED_GENERATOR" != "$EXPECTED_GENERATOR" ]; then
        GENERATOR_MISMATCH=1
        echo "==> Existing cache uses generator '$CACHED_GENERATOR'; expected '$EXPECTED_GENERATOR'."
    fi
    while IFS='=' read -r key want; do
        [ -n "$key" ] || continue
        got="$(cache_value "$CMAKE_CACHE" "$key")"
        if [ "$got" != "$want" ]; then
            FEATURE_MISMATCH=1
            echo "==> Existing cache has $key=${got:-<missing>}; expected $want."
        fi
    done <<EOF
$EXPECTED_FEATURES
EOF
fi

if [ "$FORCE_CONFIGURE" -eq 1 ] || [ "$GENERATOR_MISMATCH" -eq 1 ] || [ "$FEATURE_MISMATCH" -eq 1 ]; then
    reset_build_dir "$BUILD_DIR"
fi

if [ ! -f "$CMAKE_CACHE" ]; then
    # Build-dir reset alone misses stale per-dependency subbuild generator
    # caches under the FetchContent base dir; clear those too before configure.
    reset_stale_fetchcontent "$FETCHCONTENT_BASE_DIR" "$EXPECTED_GENERATOR"

    if [ -n "$TOOLSET_VERSION" ]; then
        echo "==> Configuring CMake ($EXPECTED_GENERATOR, x64, toolset $TOOLSET_VERSION)..."
    else
        echo "==> Configuring CMake ($EXPECTED_GENERATOR, x64)..."
    fi

    CMAKE_ARGS=(-S "$(winpath "$REPO_ROOT")" -B "$(winpath "$BUILD_DIR")" -G "$EXPECTED_GENERATOR" -A x64)
    [ -n "$TOOLSET_VERSION" ] && CMAKE_ARGS+=(-T "version=$TOOLSET_VERSION")
    CMAKE_ARGS+=(
        -DSMATCHET_WITH_LUA_AUTOMATION=ON
        -DSMATCHET_WITH_MCP=OFF
        -DSMATCHET_WITH_AI=OFF
        -DSMATCHET_WITH_WHISPER=OFF
        -DSMATCHET_WHISPER_LOCAL_BACKEND=OFF
        "-DFETCHCONTENT_BASE_DIR=$(winpath "$FETCHCONTENT_BASE_DIR")"
        "-DSMATCHET_UNREAL_THIRDPARTY_DIR=$(winpath "$SOURCE_PLUGIN_DIR/ThirdParty/Smatchet")"
    )
    (cd "$REPO_ROOT" && cmake "${CMAKE_ARGS[@]}") || die "cmake configure failed."
fi

echo "==> Building packaging target SmatchetPackageUnrealLibs_DX12..."
(cd "$REPO_ROOT" && cmake --build "$(winpath "$BUILD_DIR")" --config "$CONFIGURATION" \
    --target SmatchetPackageUnrealLibs_DX12) || die "cmake build failed."

echo "==> Packaged into: $SOURCE_PLUGIN_DIR/ThirdParty/Smatchet"

if [ "$PACKAGE_ONLY" -eq 1 ]; then
    echo "==> --package-only: skipping copy to Unreal project."
    exit 0
fi

[ -d "$PROJECT_ROOT" ] || die "target Unreal project folder not found: $PROJECT_ROOT"

DEST_PLUGIN_DIR="$PROJECT_ROOT/Plugins/$PLUGIN_NAME"
mkdir -p "$PROJECT_ROOT/Plugins" || die "cannot create $PROJECT_ROOT/Plugins"

if [ -d "$DEST_PLUGIN_DIR" ]; then
    echo "==> Removing existing deployed plugin: $DEST_PLUGIN_DIR"
    rm -rf "$DEST_PLUGIN_DIR" || die "cannot remove $DEST_PLUGIN_DIR"
fi

echo "==> Copying plugin to: $DEST_PLUGIN_DIR"
cp -r "$SOURCE_PLUGIN_DIR" "$DEST_PLUGIN_DIR" || die "plugin copy failed."

echo "==> Done."
echo "    Next: rebuild the plugin/editor (e.g. scripts/dev/local/rebuild-testproject-plugin.sh or Build.bat)."
