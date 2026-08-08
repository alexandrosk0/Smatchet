#!/usr/bin/env bash
# scripts/publish/release-github.sh — build, package and (optionally) publish
# Smatchet release artifacts.
#
# Bash port of the retired scripts/publish/release_github.ps1. Windows-only in
# practice (MSVC presets, Inno Setup, signtool), but it runs under Git Bash so
# the whole publish path is one language with the rest of the toolchain.
#
# Artifacts:
#   - Standalone zip (Smatchet.exe + runtime files)
#   - Light standalone zip (Smatchet-Light.exe + runtime files)
#   - Unreal plugin zip (Source/UnrealPlugins/SmatchetImGuiPlugin packaged tree)
#   - Fab submission bundle
#   - Source zip (git archive, filtered)
#
# Examples:
#   bash scripts/publish/release-github.sh --tag v1.2.3
#   bash scripts/publish/release-github.sh --tag v1.2.3 --sign
#   bash scripts/publish/release-github.sh --tag v1.2.3 --sign --publish --draft --notes-file RELEASE_NOTES.md
#   bash scripts/publish/release-github.sh --tag v1.2.3 --arch arm64
#
# --publish requires --sign (unsigned public releases trip SmartScreen and the
# in-app updater refuses unverified installers); --allow-unsigned-publish
# overrides knowingly.
#
# --arch arm64 builds the ninja-publish-msvc-arm64 preset (run inside an arm64
# MSVC env, e.g. SMATCHET_MSVC_ARCH=arm64 via scripts/dev/with-msvc-env.sh),
# tags artifacts with -arm64, and emits an ARM64 Inno installer. It skips the
# Unreal package (descoped on ARM64) and the light standalone (no arm64 preset).
#
# Skips `cmake --preset` when the preset build dir already has CMakeCache.txt
# (unless --force-configure).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=scripts/common/smatchet-cmake-common.sh
. "$REPO_ROOT/scripts/common/smatchet-cmake-common.sh"

TAG=""
RELEASE_NAME=""
NOTES=""
NOTES_FILE=""
STANDALONE_PRESET="ninja-publish-msvc"
LIGHT_STANDALONE_PRESET="ninja-publish-light-msvc"
UNREAL_BUILD_PRESET="ninja-iter-unreal-msvc"
ARCH="x64"
OUT_DIR=""
SKIP_BUILD=0
SKIP_INSTALLER=0
SKIP_STANDALONE=0
SKIP_LIGHT_STANDALONE=0
SKIP_UNREAL=0
SKIP_FAB_BUNDLE=0
SKIP_SOURCE_ZIP=0
ALLOW_DIRTY=0
DRAFT=0
PRERELEASE=0
PUBLISH=0
CLOBBER=0
FORCE_CONFIGURE=0
SIGN=0
ALLOW_UNSIGNED_PUBLISH=0
SIGNTOOL_PATH=""
SIGNING_CERT_PATH=""
SIGNING_CERT_PASSWORD=""
SIGNING_CERT_THUMBPRINT=""
SIGNING_CERT_SUBJECT=""
TIMESTAMP_URL=""
USE_MACHINE_CERT_STORE=0

usage() {
    cat <<'EOF'
Usage: bash scripts/publish/release-github.sh [options]

  --tag <tag>                  Release tag (default: canonical vX.Y.Z, or a
                               -local-<stamp> variant when not publishing)
  --release-name <name>        GitHub release title
  --notes <text>               Release notes body
  --notes-file <path>          Release notes from a file (mutually exclusive
                               with --notes)
  --standalone-preset <name>       default: ninja-publish-msvc
  --light-standalone-preset <name> default: ninja-publish-light-msvc
  --unreal-build-preset <name>     default: ninja-iter-unreal-msvc
  --arch <x64|arm64>           Target architecture (default: x64)
  --out-dir <path>             Output root (default: out/releases/<tag>)

  --skip-build --skip-installer --skip-standalone --skip-light-standalone
  --skip-unreal --skip-fab-bundle --skip-source-zip
  --allow-dirty --force-configure
  --publish --draft --prerelease --clobber

  --sign                       Authenticode-sign the portable payload + installer
  --allow-unsigned-publish     Knowingly publish unsigned artifacts
  --signtool-path <path>       signtool.exe override
  --signing-certificate-path <path>        PFX file
  --signing-certificate-password <secret>  PFX password
  --signing-certificate-thumbprint <sha1>  Store certificate by SHA1
  --signing-certificate-subject <name>     Store certificate by subject
  --timestamp-url <url>        RFC3161 timestamp server
  --use-machine-certificate-store          Use LocalMachine store (signtool /sm)

  -h, --help                   Show this help
EOF
}

# `shift 2` fails WITHOUT shifting when the option value is missing; under
# `set -e` that aborts with no explanation. Reject it up front with a message.
need_value() {
    [ "$1" -ge 2 ] || { echo "$(basename "$0"): $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --tag)                       need_value $# "$1"; TAG="$2"; shift 2 ;;
        --tag=*)                     TAG="${1#*=}"; shift ;;
        --release-name)              need_value $# "$1"; RELEASE_NAME="$2"; shift 2 ;;
        --release-name=*)            RELEASE_NAME="${1#*=}"; shift ;;
        --notes)                     need_value $# "$1"; NOTES="$2"; shift 2 ;;
        --notes=*)                   NOTES="${1#*=}"; shift ;;
        --notes-file)                need_value $# "$1"; NOTES_FILE="$2"; shift 2 ;;
        --notes-file=*)              NOTES_FILE="${1#*=}"; shift ;;
        --standalone-preset)         need_value $# "$1"; STANDALONE_PRESET="$2"; shift 2 ;;
        --standalone-preset=*)       STANDALONE_PRESET="${1#*=}"; shift ;;
        --light-standalone-preset)   need_value $# "$1"; LIGHT_STANDALONE_PRESET="$2"; shift 2 ;;
        --light-standalone-preset=*) LIGHT_STANDALONE_PRESET="${1#*=}"; shift ;;
        --unreal-build-preset)       need_value $# "$1"; UNREAL_BUILD_PRESET="$2"; shift 2 ;;
        --unreal-build-preset=*)     UNREAL_BUILD_PRESET="${1#*=}"; shift ;;
        --arch)                      need_value $# "$1"; ARCH="$2"; shift 2 ;;
        --arch=*)                    ARCH="${1#*=}"; shift ;;
        --out-dir)                   need_value $# "$1"; OUT_DIR="$2"; shift 2 ;;
        --out-dir=*)                 OUT_DIR="${1#*=}"; shift ;;
        --signtool-path)             need_value $# "$1"; SIGNTOOL_PATH="$2"; shift 2 ;;
        --signtool-path=*)           SIGNTOOL_PATH="${1#*=}"; shift ;;
        --signing-certificate-path)         need_value $# "$1"; SIGNING_CERT_PATH="$2"; shift 2 ;;
        --signing-certificate-path=*)       SIGNING_CERT_PATH="${1#*=}"; shift ;;
        --signing-certificate-password)     need_value $# "$1"; SIGNING_CERT_PASSWORD="$2"; shift 2 ;;
        --signing-certificate-password=*)   SIGNING_CERT_PASSWORD="${1#*=}"; shift ;;
        --signing-certificate-thumbprint)   need_value $# "$1"; SIGNING_CERT_THUMBPRINT="$2"; shift 2 ;;
        --signing-certificate-thumbprint=*) SIGNING_CERT_THUMBPRINT="${1#*=}"; shift ;;
        --signing-certificate-subject)      need_value $# "$1"; SIGNING_CERT_SUBJECT="$2"; shift 2 ;;
        --signing-certificate-subject=*)    SIGNING_CERT_SUBJECT="${1#*=}"; shift ;;
        --timestamp-url)             need_value $# "$1"; TIMESTAMP_URL="$2"; shift 2 ;;
        --timestamp-url=*)           TIMESTAMP_URL="${1#*=}"; shift ;;
        --skip-build)                SKIP_BUILD=1; shift ;;
        --skip-installer)            SKIP_INSTALLER=1; shift ;;
        --skip-standalone)           SKIP_STANDALONE=1; shift ;;
        --skip-light-standalone)     SKIP_LIGHT_STANDALONE=1; shift ;;
        --skip-unreal)               SKIP_UNREAL=1; shift ;;
        --skip-fab-bundle)           SKIP_FAB_BUNDLE=1; shift ;;
        --skip-source-zip)           SKIP_SOURCE_ZIP=1; shift ;;
        --allow-dirty)               ALLOW_DIRTY=1; shift ;;
        --draft)                     DRAFT=1; shift ;;
        --prerelease)                PRERELEASE=1; shift ;;
        --publish)                   PUBLISH=1; shift ;;
        --clobber)                   CLOBBER=1; shift ;;
        --force-configure)           FORCE_CONFIGURE=1; shift ;;
        --sign)                      SIGN=1; shift ;;
        --allow-unsigned-publish)    ALLOW_UNSIGNED_PUBLISH=1; shift ;;
        --use-machine-certificate-store) USE_MACHINE_CERT_STORE=1; shift ;;
        -h|--help)                   usage; exit 0 ;;
        *)
            echo "release-github: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

die() { echo "release-github: $*" >&2; exit 1; }
stage() { printf '\n==> %s\n' "$1"; }

case "$ARCH" in
    x64|arm64) ;;
    *) die "--arch must be x64 or arm64 (got '$ARCH')." ;;
esac

PYTHON="$(smatchet_python)" || die "python is required (zip/JSON helpers)."

# Native Windows tools (ISCC, signtool) need Windows-form paths; MSYS argument
# translation is not dependable for /D-style switches, so convert explicitly.
winpath() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    else
        printf '%s\n' "$1"
    fi
}

# Run a native Windows exe without MSYS argument translation. Load-bearing for
# the Windows `/switch` convention: Git Bash rewrites a whole argument that
# looks like an absolute POSIX path, so `/DMyAppVersion=0.1.0` reaches the exe
# as `C:/Program Files/Git/DMyAppVersion=0.1.0` — no longer a switch. ISCC then
# counts it as a second script filename and aborts ("You may not specify more
# than one script filename"); signtool would see `/fd` as a file path. Values
# are already Windows-form via winpath(), so suppressing translation is safe.
native_exec() {
    MSYS_NO_PATHCONV=1 "$@"
}

require_tool() {
    command -v "$1" >/dev/null 2>&1 || die "Required tool not found on PATH: $1"
}

# --- Small filesystem helpers -------------------------------------------------

abs_path() {
    "$PYTHON" -c 'import os,sys; print(os.path.abspath(sys.argv[1]))' "$1"
}

new_clean_directory() {
    local path resolved root
    path="$1"
    [ -n "${path//[[:space:]]/}" ] || die "Refusing to clean empty path."
    resolved="$(abs_path "$path")"
    root="$("$PYTHON" -c 'import os,sys; print(os.path.splitdrive(sys.argv[1])[0] + os.sep)' "$resolved")"
    if [ "$(printf '%s' "$resolved" | tr '\\' '/')" = "$(printf '%s' "$root" | tr '\\' '/')" ]; then
        die "Refusing to clean root path: $resolved"
    fi
    rm -rf "$path"
    mkdir -p "$path"
}

remove_if_present() { [ -e "$1" ] && rm -rf "$1" || true; }

# Compress-Archive equivalent: zip the *contents* of a directory into the
# archive root (no top-level folder), deterministic ordering.
compress_directory() {
    local src="$1" zip_path="$2"
    rm -f "$zip_path"
    "$PYTHON" - "$src" "$zip_path" <<'PY'
import os, sys, zipfile
src, dest = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
    for root, dirs, files in os.walk(src):
        dirs.sort()
        for name in sorted(files):
            full = os.path.join(root, name)
            zf.write(full, os.path.relpath(full, src).replace(os.sep, "/"))
PY
    [ -f "$zip_path" ] || die "Failed to create zip archive: $zip_path"
}

expand_archive() {
    "$PYTHON" - "$1" "$2" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as zf:
    zf.extractall(sys.argv[2])
PY
}

copy_if_present() {
    local src="$1" dest="$2"
    [ -e "$src" ] || return 0
    mkdir -p "$(dirname "$dest")"
    cp -RF "$src" "$dest" 2>/dev/null || cp -R "$src" "$dest"
}

copy_standalone_runtime_payload() {
    local src_dir="$1" dest_dir="$2"
    if [ -d "$src_dir/Scripts" ]; then
        mkdir -p "$dest_dir/Scripts"
        find "$src_dir/Scripts" -maxdepth 1 -type f -name '*.lua' -exec cp -f {} "$dest_dir/Scripts/" \;
        if [ -d "$src_dir/Scripts/art" ]; then
            rm -rf "$dest_dir/Scripts/art"
            cp -R "$src_dir/Scripts/art" "$dest_dir/Scripts/art"
        fi
    fi
    if [ -d "$src_dir/Locales" ]; then
        rm -rf "$dest_dir/Locales"
        cp -R "$src_dir/Locales" "$dest_dir/Locales"
    fi
    copy_if_present "$src_dir/default_settings.json" "$dest_dir/default_settings.json"
}

# --- Tool discovery -----------------------------------------------------------

# PATH -> the three fixed install dirs -> the Uninstall registry keys. The CI
# installer step's warning text names this chain, so keep all three tiers.
resolve_inno_setup_compiler() {
    local candidate hive install_loc
    if command -v ISCC.exe >/dev/null 2>&1; then
        command -v ISCC.exe
        return 0
    fi
    for candidate in \
        "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
        "/c/Program Files/Inno Setup 6/ISCC.exe" \
        "${LOCALAPPDATA:-}/Programs/Inno Setup 6/ISCC.exe"
    do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    command -v reg >/dev/null 2>&1 || return 1
    for hive in \
        'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall' \
        'HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall' \
        'HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'
    do
        # One `reg query /s` per hive, then pair DisplayName "Inno Setup*" with
        # the sibling InstallLocation inside the same key block.
        while IFS= read -r install_loc; do
            [ -n "$install_loc" ] || continue
            candidate="$(cygpath -u "$install_loc" 2>/dev/null || printf '%s' "$install_loc")"
            candidate="${candidate%/}/ISCC.exe"
            if [ -f "$candidate" ]; then
                printf '%s\n' "$candidate"
                return 0
            fi
        done < <(reg query "$hive" /s /v DisplayName 2>/dev/null | inno_install_locations "$hive")
    done
    return 1
}

# Helper for resolve_inno_setup_compiler: reads a `reg query /s /v DisplayName`
# dump on stdin and prints the InstallLocation of every "Inno Setup*" entry.
inno_install_locations() {
    local hive="$1" key="" line loc
    # `reg query` accepts the short hive name on input but echoes key lines with
    # the full one (HKLM\... in, HKEY_LOCAL_MACHINE\... out), so expand before
    # matching or no key line is ever recognised.
    case "$hive" in
        HKLM\\*) hive="HKEY_LOCAL_MACHINE${hive#HKLM}" ;;
        HKCU\\*) hive="HKEY_CURRENT_USER${hive#HKCU}" ;;
        HKCR\\*) hive="HKEY_CLASSES_ROOT${hive#HKCR}" ;;
        HKU\\*) hive="HKEY_USERS${hive#HKU}" ;;
    esac
    while IFS= read -r line; do
        line="${line%$'\r'}"
        case "$line" in
            "$hive"\\*) key="$line" ;;
            *DisplayName*REG_SZ*Inno\ Setup*)
                [ -n "$key" ] || continue
                loc="$(reg query "$key" /v InstallLocation 2>/dev/null \
                    | sed -n 's/.*InstallLocation[[:space:]]*REG_[A-Z_]*[[:space:]]*//p' \
                    | head -1)"
                loc="${loc%$'\r'}"
                [ -n "${loc//[[:space:]]/}" ] && printf '%s\n' "$loc"
                ;;
        esac
    done
    return 0
}

resolve_signtool() {
    local preferred="$1" full found
    if [ -n "$preferred" ]; then
        full="$(abs_path "$preferred")"
        [ -f "$full" ] || die "SignTool path not found: $preferred"
        printf '%s\n' "$full"
        return 0
    fi
    if command -v signtool.exe >/dev/null 2>&1; then
        command -v signtool.exe
        return 0
    fi
    found="$(find "/c/Program Files (x86)/Windows Kits/10/bin" -type f -name signtool.exe 2>/dev/null \
        | grep -E '/x64/signtool\.exe$' | sort -r | head -1 || true)"
    if [ -n "$found" ]; then
        printf '%s\n' "$found"
        return 0
    fi
    local app_cert="/c/Program Files (x86)/Windows Kits/10/App Certification Kit/signtool.exe"
    if [ -f "$app_cert" ]; then
        printf '%s\n' "$app_cert"
        return 0
    fi
    die "Unable to locate signtool.exe. Install the Windows SDK or pass --signtool-path."
}

# --- Signing configuration ----------------------------------------------------

SIGN_TOOL=""
SIGN_CERT_PATH=""
SIGN_CERT_PASSWORD=""
SIGN_CERT_THUMBPRINT=""
SIGN_CERT_SUBJECT=""
SIGN_TIMESTAMP_URL=""
SIGN_USE_MACHINE_STORE=0

configure_signing() {
    [ "$SIGN" -eq 1 ] || return 0

    SIGN_TOOL="$(resolve_signtool "${SIGNTOOL_PATH:-${SMATCHET_SIGNTOOL_PATH:-}}")"

    if [ -n "$SIGNING_CERT_PATH" ]; then
        SIGN_CERT_PATH="$(abs_path "$SIGNING_CERT_PATH")"
    elif [ -n "${SMATCHET_SIGN_PFX_PATH:-}" ]; then
        SIGN_CERT_PATH="$(abs_path "$SMATCHET_SIGN_PFX_PATH")"
    fi
    SIGN_CERT_PASSWORD="${SIGNING_CERT_PASSWORD:-${SMATCHET_SIGN_PFX_PASSWORD:-}}"
    SIGN_CERT_THUMBPRINT="${SIGNING_CERT_THUMBPRINT:-${SMATCHET_SIGN_CERT_SHA1:-}}"
    SIGN_CERT_SUBJECT="${SIGNING_CERT_SUBJECT:-${SMATCHET_SIGN_CERT_SUBJECT:-}}"
    SIGN_TIMESTAMP_URL="${TIMESTAMP_URL:-${SMATCHET_SIGN_TIMESTAMP_URL:-http://timestamp.digicert.com}}"
    if [ "$USE_MACHINE_CERT_STORE" -eq 1 ]; then
        SIGN_USE_MACHINE_STORE=1
    else
        case "$(printf '%s' "${SMATCHET_SIGN_USE_MACHINE_STORE:-}" | tr '[:upper:]' '[:lower:]')" in
            1|true|yes) SIGN_USE_MACHINE_STORE=1 ;;
        esac
    fi

    if [ -n "$SIGN_CERT_PATH" ] && [ ! -f "$SIGN_CERT_PATH" ]; then
        die "Signing certificate not found: $SIGN_CERT_PATH"
    fi

    local selectors=0
    [ -n "$SIGN_CERT_PATH" ] && selectors=$((selectors + 1))
    [ -n "$SIGN_CERT_THUMBPRINT" ] && selectors=$((selectors + 1))
    [ -n "$SIGN_CERT_SUBJECT" ] && selectors=$((selectors + 1))
    if [ "$selectors" -ne 1 ]; then
        die "Signing requires exactly one certificate selector: PFX path, certificate thumbprint, or certificate subject."
    fi
}

# Emits the signtool argv (one arg per line) for a single file.
signtool_argv() {
    local file="$1"
    printf '%s\n' sign /fd SHA256 /td SHA256 /tr "$SIGN_TIMESTAMP_URL"
    if [ -n "$SIGN_CERT_PATH" ]; then
        printf '%s\n' /f "$(winpath "$SIGN_CERT_PATH")"
        [ -n "$SIGN_CERT_PASSWORD" ] && printf '%s\n' /p "$SIGN_CERT_PASSWORD"
    elif [ -n "$SIGN_CERT_THUMBPRINT" ]; then
        [ "$SIGN_USE_MACHINE_STORE" -eq 1 ] && printf '%s\n' /sm
        printf '%s\n' /sha1 "$SIGN_CERT_THUMBPRINT"
    else
        [ "$SIGN_USE_MACHINE_STORE" -eq 1 ] && printf '%s\n' /sm
        printf '%s\n' /n "$SIGN_CERT_SUBJECT"
    fi
    printf '%s\n' "$(winpath "$file")"
}

assert_signature_present() {
    native_exec "$SIGN_TOOL" verify /pa "$(winpath "$1")" >/dev/null 2>&1 \
        || die "File is not authenticode signed: $1"
}

sign_files() {
    local file argv
    for file in "$@"; do
        [ -f "$file" ] || die "Cannot sign missing file: $file"
        mapfile -t argv < <(signtool_argv "$file")
        native_exec "$SIGN_TOOL" "${argv[@]}" || die "signtool failed for $file"
        assert_signature_present "$file"
    done
}

# `$q` is the Inno preprocessor's double-quote constant; embedded quotes double.
quote_inno_value() {
    printf '$q%s$q\n' "$(printf '%s' "$1" | sed 's/"/$q$q/g')"
}

inno_signtool_definition() {
    local description="$1" parts
    parts="$(quote_inno_value "$(winpath "$SIGN_TOOL")") sign /fd SHA256 /td SHA256"
    parts="$parts /tr $(quote_inno_value "$SIGN_TIMESTAMP_URL")"
    parts="$parts /d $(quote_inno_value "$description")"
    if [ -n "$SIGN_CERT_PATH" ]; then
        parts="$parts /f $(quote_inno_value "$(winpath "$SIGN_CERT_PATH")")"
        [ -n "$SIGN_CERT_PASSWORD" ] && parts="$parts /p $(quote_inno_value "$SIGN_CERT_PASSWORD")"
    elif [ -n "$SIGN_CERT_THUMBPRINT" ]; then
        [ "$SIGN_USE_MACHINE_STORE" -eq 1 ] && parts="$parts /sm"
        parts="$parts /sha1 $SIGN_CERT_THUMBPRINT"
    else
        [ "$SIGN_USE_MACHINE_STORE" -eq 1 ] && parts="$parts /sm"
        parts="$parts /n $(quote_inno_value "$SIGN_CERT_SUBJECT")"
    fi
    printf '%s $f\n' "$parts"
}

# --- Build / staging ----------------------------------------------------------

standalone_exe_path() {
    local build_dir="$1" exe_name="$2" candidate found
    for candidate in "$build_dir/$exe_name" "$build_dir/Release/$exe_name"; do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    found="$(find "$build_dir" -type f -name "$exe_name" -printf '%T@\t%p\n' 2>/dev/null \
        | sort -rn | head -1 | cut -f2- || true)"
    [ -n "$found" ] || die "Could not find $exe_name under $build_dir"
    printf '%s\n' "$found"
}

new_standalone_portable_zip() {
    local build_dir="$1" stage_dir="$2" exe_name="$3" zip_path="$4"
    local exe_path exe_dir dll signables

    new_clean_directory "$stage_dir"
    exe_path="$(standalone_exe_path "$build_dir" "$exe_name")"
    cp -f "$exe_path" "$stage_dir/$exe_name"

    exe_dir="$(dirname "$exe_path")"
    while IFS= read -r dll; do
        [ -n "$dll" ] && cp -f "$dll" "$stage_dir/$(basename "$dll")"
    done < <(find "$exe_dir" -maxdepth 1 -type f -name '*.dll' 2>/dev/null || true)

    copy_standalone_runtime_payload "$exe_dir" "$stage_dir"
    [ -f "$LICENSE_PATH" ] && cp -f "$LICENSE_PATH" "$stage_dir/LICENSE"
    [ -f "$README_PATH" ] && cp -f "$README_PATH" "$stage_dir/README.md"

    if [ "$SIGN" -eq 1 ]; then
        mapfile -t signables < <(find "$stage_dir" -type f \( -iname '*.exe' -o -iname '*.dll' \) 2>/dev/null | sort)
        [ "${#signables[@]}" -gt 0 ] && sign_files "${signables[@]}"
    fi

    compress_directory "$stage_dir" "$zip_path"
}

# --- Preflight ----------------------------------------------------------------

PRESET_FILE="$REPO_ROOT/CMakePresets.json"
PLUGIN_ROOT="$REPO_ROOT/Source/UnrealPlugins/SmatchetImGuiPlugin"
LICENSE_PATH="$REPO_ROOT/LICENSE"
README_PATH="$REPO_ROOT/README.md"
INSTALLER_SCRIPT="$SCRIPT_DIR/installer/SmatchetStandalone.iss"
PLUGIN_INSTALL_SCRIPT="$SCRIPT_DIR/install-unreal-plugin.sh"
PLUGIN_INSTALL_GUIDE="$SCRIPT_DIR/INSTALL_UNREAL_PLUGIN.md"

configure_signing

# Publishing unsigned artifacts is opt-in, never accidental: unsigned binaries trip
# SmartScreen for every user, and the in-app updater refuses to launch an installer
# that fails Authenticode verification (AttachmentAppUpdateService), so an unsigned
# published release breaks auto-update. Local/CI bundles without --publish stay
# allowed unsigned (e.g. the ARM64 installer smoke job).
if [ "$PUBLISH" -eq 1 ] && [ "$SIGN" -eq 0 ]; then
    if [ "$ALLOW_UNSIGNED_PUBLISH" -eq 0 ]; then
        die "Publishing requires code signing: pass --sign (see scripts/publish/SIGNING.md). To knowingly publish an unsigned, non-production release, pass --allow-unsigned-publish."
    fi
    echo "WARNING: Publishing UNSIGNED artifacts (--allow-unsigned-publish): users will see SmartScreen warnings and the in-app updater will refuse this installer." >&2
fi

[ -f "$PRESET_FILE" ] || die "Missing CMakePresets.json at repo root: $PRESET_FILE"
[ -d "$PLUGIN_ROOT" ] || die "Missing Unreal plugin directory: $PLUGIN_ROOT"

require_tool cmake
require_tool git
[ "$PUBLISH" -eq 1 ] && require_tool gh

if [ -n "$NOTES" ] && [ -n "$NOTES_FILE" ]; then
    die "Use either --notes or --notes-file, not both."
fi
if [ -n "$NOTES_FILE" ] && [ ! -f "$NOTES_FILE" ]; then
    die "Notes file not found: $NOTES_FILE"
fi

PROJECT_VERSION="$(smatchet_project_version "$REPO_ROOT")"
EXPECTED_TAG="$(smatchet_version_tag "$PROJECT_VERSION")"
SOURCE_PLUGIN_MANIFEST="$(smatchet_plugin_manifest_path "$REPO_ROOT")"
if ! smatchet_plugin_manifest_matches_version "$SOURCE_PLUGIN_MANIFEST" "$PROJECT_VERSION"; then
    die "Plugin manifest is out of sync with CMake version $PROJECT_VERSION. Run scripts/publish/sync-release-version.sh or update the manifest before releasing."
fi

EFFECTIVE_TAG="$TAG"
if [ -z "$EFFECTIVE_TAG" ]; then
    if [ "$PUBLISH" -eq 1 ]; then
        EFFECTIVE_TAG="$EXPECTED_TAG"
    else
        EFFECTIVE_TAG="$EXPECTED_TAG-local-$(date +%Y%m%d-%H%M%S)"
    fi
fi
if [ "$PUBLISH" -eq 1 ] && [ "$EFFECTIVE_TAG" != "$EXPECTED_TAG" ]; then
    die "Publish tag '$EFFECTIVE_TAG' does not match canonical version tag '$EXPECTED_TAG'."
fi

if [ -z "$OUT_DIR" ]; then
    OUT_DIR="$REPO_ROOT/out/releases/$EFFECTIVE_TAG"
elif [ "${OUT_DIR#/}" = "$OUT_DIR" ] && [ "${OUT_DIR#?:}" = "$OUT_DIR" ]; then
    OUT_DIR="$REPO_ROOT/$OUT_DIR"
fi
OUT_DIR="$(abs_path "$OUT_DIR")"
ASSETS_DIR="$OUT_DIR/assets"
STAGING_DIR="$OUT_DIR/staging"
STANDALONE_STAGE="$STAGING_DIR/standalone"
LIGHT_STANDALONE_STAGE="$STAGING_DIR/standalone-light"
PLUGIN_STAGE="$STAGING_DIR/plugin"
FAB_STAGE="$STAGING_DIR/fab-submission"
ASSET_PATHS=()

# --- Architecture wiring ------------------------------------------------------
# x64 (default) keeps every artifact name + Inno arch byte-identical to before.
# arm64 packages the native Windows-on-ARM build: tag artifacts -arm64 and pass
# the arm64 Inno architecture so the installer targets the 64-bit view on WoA.
#
# Reconcile --arch with the (possibly custom) --standalone-preset first: a
# *-arm64 preset must pair with --arch arm64 and vice versa, or the build tree's
# arch and the artifact/installer arch labels diverge silently. The default
# preset auto-upgrades to the arm64 tree; any other mismatch is a hard error.
PRESET_IS_ARM64=0
case "$STANDALONE_PRESET" in *-arm64) PRESET_IS_ARM64=1 ;; esac

if [ "$ARCH" = "arm64" ] && [ "$PRESET_IS_ARM64" -eq 0 ]; then
    if [ "$STANDALONE_PRESET" = "ninja-publish-msvc" ]; then
        STANDALONE_PRESET="ninja-publish-msvc-arm64"
    else
        die "--arch arm64 requires an ARM64 standalone preset; got '$STANDALONE_PRESET'. Use ninja-publish-msvc-arm64 (or omit --standalone-preset)."
    fi
elif [ "$ARCH" = "x64" ] && [ "$PRESET_IS_ARM64" -eq 1 ]; then
    die "--standalone-preset '$STANDALONE_PRESET' targets ARM64 but --arch is x64. Pass --arch arm64 so artifact names and the installer match the build."
fi

# Unreal is descoped on ARM64 and there is no arm64 light preset yet, so both are
# skipped for arm64 to keep the run green end-to-end.
ARCH_TOKEN=""
INNO_ARCH="x64compatible"
if [ "$ARCH" = "arm64" ]; then
    ARCH_TOKEN="-arm64"
    INNO_ARCH="arm64"
    if [ "$SKIP_UNREAL" -eq 0 ]; then
        echo "  --arch arm64: skipping Unreal package (descoped on ARM64)."
        SKIP_UNREAL=1
    fi
    if [ "$SKIP_LIGHT_STANDALONE" -eq 0 ]; then
        echo "  --arch arm64: skipping light standalone (no arm64 light preset yet)."
        SKIP_LIGHT_STANDALONE=1
    fi
fi

ISCC_EXE=""
if [ "$SKIP_STANDALONE" -eq 0 ] && [ "$SKIP_INSTALLER" -eq 0 ]; then
    ISCC_EXE="$(resolve_inno_setup_compiler || true)"
    [ -n "$ISCC_EXE" ] || die "Inno Setup 6 compiler (ISCC.exe) is required for installer packaging. Install Inno Setup or re-run with --skip-installer."
fi

stage "Validating git context"
git -C "$REPO_ROOT" rev-parse --is-inside-work-tree >/dev/null
if [ "$ALLOW_DIRTY" -eq 0 ] && [ -n "$(git -C "$REPO_ROOT" status --porcelain)" ]; then
    die "Working tree not clean. Commit/stash changes or re-run with --allow-dirty."
fi
if [ "$PUBLISH" -eq 1 ]; then
    (cd "$REPO_ROOT" && gh auth status)
    (cd "$REPO_ROOT" && gh repo view >/dev/null)
    git -C "$REPO_ROOT" rev-parse -q --verify "refs/tags/$EFFECTIVE_TAG" >/dev/null \
        || die "Tag '$EFFECTIVE_TAG' not found locally. Create and push tag before publish."
fi

stage "Preparing output directories"
new_clean_directory "$OUT_DIR"
new_clean_directory "$ASSETS_DIR"
new_clean_directory "$STAGING_DIR"

STANDALONE_BUILD_DIR="$(abs_path "$(smatchet_configure_preset_binary_dir "$REPO_ROOT" "$STANDALONE_PRESET")")"
LIGHT_STANDALONE_BUILD_DIR="$(abs_path "$(smatchet_configure_preset_binary_dir "$REPO_ROOT" "$LIGHT_STANDALONE_PRESET")")"
UNREAL_BUILD_DIR="$(abs_path "$(smatchet_configure_preset_binary_dir "$REPO_ROOT" "$UNREAL_BUILD_PRESET")")"

configure_and_build() {
    local preset="$1" build_dir="$2"
    if [ ! -f "$build_dir/CMakeCache.txt" ] || [ "$FORCE_CONFIGURE" -eq 1 ]; then
        (cd "$REPO_ROOT" && cmake --preset "$preset")
    fi
    (cd "$REPO_ROOT" && cmake --build --preset "$preset")
}

if [ "$SKIP_BUILD" -eq 0 ]; then
    stage "Configuring and building standalone ($STANDALONE_PRESET)"
    configure_and_build "$STANDALONE_PRESET" "$STANDALONE_BUILD_DIR"

    if [ "$SKIP_LIGHT_STANDALONE" -eq 0 ]; then
        stage "Configuring and building light standalone ($LIGHT_STANDALONE_PRESET)"
        configure_and_build "$LIGHT_STANDALONE_PRESET" "$LIGHT_STANDALONE_BUILD_DIR"
    fi

    if [ "$SKIP_UNREAL" -eq 0 ] && [ "$UNREAL_BUILD_PRESET" != "$STANDALONE_PRESET" ]; then
        stage "Configuring and building Unreal package target ($UNREAL_BUILD_PRESET)"
        configure_and_build "$UNREAL_BUILD_PRESET" "$UNREAL_BUILD_DIR"
    fi
fi

if [ "$SKIP_STANDALONE" -eq 0 ]; then
    stage "Staging standalone artifact"
    STANDALONE_ZIP="$ASSETS_DIR/Smatchet-$EFFECTIVE_TAG-windows$ARCH_TOKEN-portable.zip"
    new_standalone_portable_zip "$STANDALONE_BUILD_DIR" "$STANDALONE_STAGE" "Smatchet.exe" "$STANDALONE_ZIP"
    ASSET_PATHS+=("$STANDALONE_ZIP")

    if [ "$SKIP_INSTALLER" -eq 0 ]; then
        stage "Building Windows installer"
        INSTALLER_BASE="Smatchet-$EFFECTIVE_TAG-windows$ARCH_TOKEN-setup"
        # Append the signing defines ONLY when signing is enabled — an empty
        # /Ssmatchetsigntool= value makes iscc abort with "Unknown option:" on
        # unsigned builds (the CI installer smoke path; real releases sign).
        iscc_args=(
            "/DMyAppVersion=$PROJECT_VERSION"
            "/DMySourceDir=$(winpath "$STANDALONE_STAGE")"
            "/DMyOutputDir=$(winpath "$ASSETS_DIR")"
            "/DMyOutputBaseFilename=$INSTALLER_BASE"
            "/DMyArchitecturesAllowed=$INNO_ARCH"
        )
        if [ "$SIGN" -eq 1 ]; then
            iscc_args+=("/DMyInnoSignTool=smatchetsigntool")
            iscc_args+=("/Ssmatchetsigntool=$(inno_signtool_definition 'Smatchet Installer')")
        fi
        iscc_args+=("$(winpath "$INSTALLER_SCRIPT")")
        native_exec "$ISCC_EXE" "${iscc_args[@]}"
        INSTALLER_PATH="$ASSETS_DIR/$INSTALLER_BASE.exe"
        [ -f "$INSTALLER_PATH" ] || die "Expected installer missing: $INSTALLER_PATH"
        [ "$SIGN" -eq 1 ] && assert_signature_present "$INSTALLER_PATH"
        ASSET_PATHS+=("$INSTALLER_PATH")
    fi
fi

if [ "$SKIP_LIGHT_STANDALONE" -eq 0 ]; then
    stage "Staging light standalone artifact"
    LIGHT_ZIP="$ASSETS_DIR/Smatchet-$EFFECTIVE_TAG-windows-light-portable.zip"
    new_standalone_portable_zip "$LIGHT_STANDALONE_BUILD_DIR" "$LIGHT_STANDALONE_STAGE" "Smatchet-Light.exe" "$LIGHT_ZIP"
    ASSET_PATHS+=("$LIGHT_ZIP")
fi

if [ "$SKIP_UNREAL" -eq 0 ]; then
    stage "Staging Unreal plugin artifact"
    new_clean_directory "$PLUGIN_STAGE"

    PLUGIN_DEST="$PLUGIN_STAGE/SmatchetImGuiPlugin"
    cp -R "$PLUGIN_ROOT" "$PLUGIN_DEST"
    for noise in Binaries Intermediate Saved .vs; do
        remove_if_present "$PLUGIN_DEST/$noise"
    done

    smatchet_set_plugin_manifest_version "$PLUGIN_DEST/SmatchetImGuiPlugin.uplugin" "$PROJECT_VERSION"
    cp -f "$PLUGIN_INSTALL_SCRIPT" "$PLUGIN_STAGE/install-unreal-plugin.sh"
    cp -f "$PLUGIN_INSTALL_GUIDE" "$PLUGIN_STAGE/INSTALL_UNREAL_PLUGIN.md"

    PLUGIN_ZIP="$ASSETS_DIR/Smatchet-$EFFECTIVE_TAG-unreal-plugin.zip"
    compress_directory "$PLUGIN_STAGE" "$PLUGIN_ZIP"
    ASSET_PATHS+=("$PLUGIN_ZIP")

    if [ "$SKIP_FAB_BUNDLE" -eq 0 ]; then
        stage "Preparing Fab submission bundle"
        new_clean_directory "$FAB_STAGE"
        PLUGIN_ZIP_NAME="$(basename "$PLUGIN_ZIP")"
        cp -f "$PLUGIN_ZIP" "$FAB_STAGE/$PLUGIN_ZIP_NAME"
        cp -f "$PLUGIN_INSTALL_GUIDE" "$FAB_STAGE/INSTALL_UNREAL_PLUGIN.md"

        # UTF-8 without a BOM plus a trailing newline — the Fab portal's importer
        # rejects a BOM'd manifest.
        "$PYTHON" - "$FAB_STAGE/fab-submission.json" "$PROJECT_VERSION" "$EXPECTED_TAG" "$PLUGIN_ZIP_NAME" <<'PY'
import collections, json, sys
dest, version, tag, asset = sys.argv[1:5]
doc = collections.OrderedDict((
    ("product", "Smatchet"),
    ("version", version),
    ("tag", tag),
    ("releaseAsset", asset),
    ("pluginFolder", "SmatchetImGuiPlugin"),
    ("pluginManifest", "SmatchetImGuiPlugin/SmatchetImGuiPlugin.uplugin"),
    ("support", collections.OrderedDict((
        ("readme", "README.md"),
        ("installGuide", "INSTALL_UNREAL_PLUGIN.md"),
    ))),
    ("submissionChecklist", [
        "Upload the plugin zip to Fab publisher portal.",
        "Verify listing version matches the canonical Smatchet version.",
        "Attach release notes and support links.",
        "Confirm the packaged plugin installs into a project Plugins folder.",
    ]),
))
with open(dest, "w", encoding="utf-8", newline="\n") as fh:
    json.dump(doc, fh, indent=4)
    fh.write("\n")
PY

        cat > "$FAB_STAGE/FAB_SUBMISSION.md" <<EOF
Fab Submission Bundle
=====================

Product: Smatchet
Version: $PROJECT_VERSION
Tag: $EXPECTED_TAG

Included files:
- $PLUGIN_ZIP_NAME
- INSTALL_UNREAL_PLUGIN.md
- fab-submission.json

Portal steps:
1. Upload the plugin zip to the Fab listing for Smatchet.
2. Set the listing version to $PROJECT_VERSION.
3. Paste the release notes or link back to the GitHub release for this version.
4. Submit for review after verifying install into a clean Unreal project.
EOF

        FAB_ZIP="$ASSETS_DIR/Smatchet-$EFFECTIVE_TAG-fab-submission.zip"
        compress_directory "$FAB_STAGE" "$FAB_ZIP"
        ASSET_PATHS+=("$FAB_ZIP")
    fi
fi

if [ "$SKIP_SOURCE_ZIP" -eq 0 ]; then
    stage "Creating source archive"
    SOURCE_ZIP="$ASSETS_DIR/Smatchet-$EFFECTIVE_TAG-source.zip"
    SOURCE_REF="HEAD"
    SOURCE_STAGE_ROOT="$OUT_DIR/stage-source"
    SOURCE_ARCHIVE_TEMP="$OUT_DIR/source-raw.zip"
    SOURCE_PREFIX_DIR="$SOURCE_STAGE_ROOT/Smatchet-$EFFECTIVE_TAG"

    if git -C "$REPO_ROOT" rev-parse -q --verify "refs/tags/$EFFECTIVE_TAG" >/dev/null; then
        SOURCE_REF="refs/tags/$EFFECTIVE_TAG"
    elif [ "$PUBLISH" -eq 1 ]; then
        die "Publish requested but tag '$EFFECTIVE_TAG' does not exist for source archive ref."
    fi

    new_clean_directory "$SOURCE_STAGE_ROOT"
    remove_if_present "$SOURCE_ARCHIVE_TEMP"
    # shellcheck disable=SC2064  # expand SOURCE_ARCHIVE_TEMP now, not at trap time
    trap "rm -f '$SOURCE_ARCHIVE_TEMP'" EXIT
    git -C "$REPO_ROOT" archive --format=zip \
        "--prefix=Smatchet-$EFFECTIVE_TAG/" \
        "--output=$SOURCE_ARCHIVE_TEMP" "$SOURCE_REF"
    expand_archive "$SOURCE_ARCHIVE_TEMP" "$SOURCE_STAGE_ROOT"

    for exclude in .claude backlog; do
        remove_if_present "$SOURCE_PREFIX_DIR/$exclude"
    done

    compress_directory "$SOURCE_STAGE_ROOT" "$SOURCE_ZIP"
    rm -f "$SOURCE_ARCHIVE_TEMP"
    trap - EXIT
    ASSET_PATHS+=("$SOURCE_ZIP")
fi

for asset in "${ASSET_PATHS[@]}"; do
    [ -f "$asset" ] || die "Expected asset missing: $asset"
done

if [ "$PUBLISH" -eq 1 ]; then
    stage "Publishing assets to GitHub release"
    cd "$REPO_ROOT"

    # `gh release view` exits non-zero for a missing release AND for auth,
    # network, API, and permission failures alike, so a bare exit-code test would
    # read a transient outage as "no release yet" and fall through to
    # `release create` — silently bypassing the --clobber guard below. Treat only
    # a positively identified not-found as absent; anything else stops the
    # publish rather than guessing at the repo's state.
    probe_err="$OUT_DIR/gh-release-probe.err"
    probe_code=0
    gh release view "$EFFECTIVE_TAG" --json url >/dev/null 2>"$probe_err" || probe_code=$?
    probe_text="$(cat "$probe_err" 2>/dev/null || true)"
    rm -f "$probe_err"

    if [ "$probe_code" -eq 0 ]; then
        release_exists=1
    elif printf '%s' "$probe_text" | grep -qiE 'release not found|HTTP 404'; then
        release_exists=0
    else
        die "Could not determine whether release '$EFFECTIVE_TAG' already exists (gh exited $probe_code): $(printf '%s' "$probe_text" | tr -d '\r')"
    fi

    if [ "$release_exists" -eq 0 ]; then
        create_args=(release create "$EFFECTIVE_TAG" "${ASSET_PATHS[@]}")
        [ -n "$RELEASE_NAME" ] && create_args+=(--title "$RELEASE_NAME")
        if [ -n "$NOTES_FILE" ]; then
            create_args+=(--notes-file "$(abs_path "$NOTES_FILE")")
        elif [ -n "$NOTES" ]; then
            create_args+=(--notes "$NOTES")
        else
            create_args+=(--generate-notes)
        fi
        [ "$DRAFT" -eq 1 ] && create_args+=(--draft)
        [ "$PRERELEASE" -eq 1 ] && create_args+=(--prerelease)
        gh "${create_args[@]}"
    else
        [ "$CLOBBER" -eq 1 ] || die "Release '$EFFECTIVE_TAG' already exists. Re-run with --clobber to overwrite assets."
        gh release upload "$EFFECTIVE_TAG" "${ASSET_PATHS[@]}" --clobber
    fi

    release_url="$(gh release view "$EFFECTIVE_TAG" --json url --jq .url)" \
        || die "Failed to fetch release URL."
    echo "Release URL: $release_url"
fi

stage "Done"
echo "Output directory: $OUT_DIR"
if [ "${#ASSET_PATHS[@]}" -gt 0 ]; then
    echo "Assets:"
    for asset in "${ASSET_PATHS[@]}"; do
        echo "  - $asset"
    done
fi
