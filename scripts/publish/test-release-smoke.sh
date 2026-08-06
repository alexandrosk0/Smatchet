#!/usr/bin/env bash
# scripts/publish/test-release-smoke.sh — end-to-end smoke test over a staged
# release directory produced by scripts/publish/release-github.sh.
#
# Bash port of the retired test_release_smoke.ps1. Windows-only (validates PE
# version resources and runs the packaged installer), run from Git Bash.
#
# Expands every published asset, validates the version stamped into each one,
# exercises the light standalone's CLI surface, asserts the source zip excludes
# agent-only directories, then delegates to test-installer-smoke.sh for the
# install/uninstall round-trip. Prints a JSON summary on success.
#
# Protocol described in scripts/publish/INSTALLER_SMOKE_TEST.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RELEASE_DIR=""
EXPECTED_VERSION=""
CHECK_INSTALLER_SIGNATURES=0
CLEAR_USER_DATA_BEFORE_INSTALL=0
KEEP_USER_DATA_AFTER_TEST=0
SKIP_LIGHT_STANDALONE=0

usage() {
    cat <<'EOF'
Usage: bash scripts/publish/test-release-smoke.sh --release-dir <dir> [options]

  --release-dir <dir>      Release root containing an assets/ directory (required)
  --expected-version <ver> Assert the version stamped into every asset
  --check-installer-signatures      Record Authenticode status during the installer test
  --clear-user-data-before-install  Delete %LOCALAPPDATA%\Smatchet before installing
  --keep-user-data-after-test       Leave user data in place when done
  --skip-light-standalone           Skip the light portable zip and its CLI checks
  -h, --help               Show this help
EOF
}

# `shift 2` fails WITHOUT shifting when the option value is missing; under
# `set -e` that aborts with no explanation. Reject it up front with a message.
need_value() {
    [ "$1" -ge 2 ] || { echo "$(basename "$0"): $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --release-dir)        need_value $# "$1"; RELEASE_DIR="$2"; shift 2 ;;
        --release-dir=*)      RELEASE_DIR="${1#*=}"; shift ;;
        --expected-version)   need_value $# "$1"; EXPECTED_VERSION="$2"; shift 2 ;;
        --expected-version=*) EXPECTED_VERSION="${1#*=}"; shift ;;
        --check-installer-signatures)     CHECK_INSTALLER_SIGNATURES=1; shift ;;
        --clear-user-data-before-install) CLEAR_USER_DATA_BEFORE_INSTALL=1; shift ;;
        --keep-user-data-after-test)      KEEP_USER_DATA_AFTER_TEST=1; shift ;;
        --skip-light-standalone)          SKIP_LIGHT_STANDALONE=1; shift ;;
        -h|--help)            usage; exit 0 ;;
        *)
            echo "test-release-smoke: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

die() { echo "test-release-smoke: $*" >&2; exit 1; }

[ -n "${RELEASE_DIR//[[:space:]]/}" ] || die "--release-dir is required."

command -v python >/dev/null 2>&1 && PYTHON=python || PYTHON=python3
command -v "$PYTHON" >/dev/null 2>&1 || die "python is required."

abs_path() { "$PYTHON" -c 'import os,sys; print(os.path.abspath(sys.argv[1]))' "$1"; }

RELEASE_ROOT="$(abs_path "$RELEASE_DIR")"
[ -d "$RELEASE_ROOT" ] || die "Release directory not found: $RELEASE_ROOT"
ASSETS_DIR="$RELEASE_ROOT/assets"
[ -d "$ASSETS_DIR" ] || die "Release assets directory not found: $ASSETS_DIR"

# First match wins, exactly as the PowerShell version's Get-ChildItem | Select-Object -First 1.
find_asset() {
    local pattern="$1" description="$2" match
    match="$(find "$ASSETS_DIR" -maxdepth 1 -type f -name "$pattern" | sort | head -1)"
    [ -n "$match" ] || die "Could not find $description ($pattern) under $ASSETS_DIR"
    printf '%s\n' "$match"
}

expand_archive() {
    local archive="$1" destination="$2"
    "$PYTHON" - "$archive" "$destination" <<'PY'
import sys, zipfile
archive, destination = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(archive) as zf:
    zf.extractall(destination)
PY
}

new_clean_directory() {
    local path="$1"
    if [ -d "$path" ]; then
        rm -rf "$path"
    fi
    mkdir -p "$path"
}

json_field() {
    local file="$1" field="$2"
    "$PYTHON" - "$file" "$field" <<'PY'
import json, sys
path, field = sys.argv[1], sys.argv[2]
with open(path, "r", encoding="utf-8-sig") as fh:
    data = json.load(fh)
value = data.get(field)
if value is None:
    sys.exit("Missing '%s' in %s" % (field, path))
print(value)
PY
}

PORTABLE_ZIP="$(find_asset '*windows-portable.zip' 'the portable zip')"
LIGHT_ZIP=""
if [ "$SKIP_LIGHT_STANDALONE" -eq 0 ]; then
    LIGHT_ZIP="$(find_asset '*windows-light-portable.zip' 'the light portable zip')"
fi
INSTALLER="$(find_asset '*windows-setup.exe' 'the installer')"
PLUGIN_ZIP="$(find_asset '*unreal-plugin.zip' 'the Unreal plugin zip')"
FAB_ZIP="$(find_asset '*fab-submission.zip' 'the Fab submission zip')"
SOURCE_ZIP="$(find_asset '*source.zip' 'the source zip')"

PORTABLE_DIR="$RELEASE_ROOT/smoke-portable"
LIGHT_DIR="$RELEASE_ROOT/smoke-portable-light"
PLUGIN_DIR="$RELEASE_ROOT/smoke-plugin"
FAB_DIR="$RELEASE_ROOT/smoke-fab"
SOURCE_DIR="$RELEASE_ROOT/smoke-source"

new_clean_directory "$PORTABLE_DIR"
expand_archive "$PORTABLE_ZIP" "$PORTABLE_DIR"
PORTABLE_EXE="$PORTABLE_DIR/Smatchet.exe"
[ -f "$PORTABLE_EXE" ] || die "Portable zip does not contain Smatchet.exe: $PORTABLE_ZIP"
PORTABLE_VERSION_JSON="$("$PYTHON" "$SCRIPT_DIR/test-windows-version-info.py" \
    "$PORTABLE_EXE" --expected-version "$EXPECTED_VERSION")"

LIGHT_VERSION_JSON="null"
LIGHT_CLI_JSON="null"
if [ "$SKIP_LIGHT_STANDALONE" -eq 0 ]; then
    new_clean_directory "$LIGHT_DIR"
    expand_archive "$LIGHT_ZIP" "$LIGHT_DIR"
    LIGHT_EXE="$LIGHT_DIR/Smatchet-Light.exe"
    [ -f "$LIGHT_EXE" ] || die "Light portable zip does not contain Smatchet-Light.exe: $LIGHT_ZIP"
    LIGHT_VERSION_JSON="$("$PYTHON" "$SCRIPT_DIR/test-windows-version-info.py" \
        "$LIGHT_EXE" --expected-version "$EXPECTED_VERSION" \
        --expected-file-description "Smatchet Light Standalone")"
    [ -d "$LIGHT_DIR/Scripts" ] || die "Light portable zip does not contain a Scripts directory."

    # The light build must expose the command surface minus every ai.* command,
    # so an unknown-command exit 2 from ai.send-once is the positive assertion
    # that the AI surface really was compiled out.
    commands_list_output=""
    commands_list_exit=0
    commands_list_output="$("$LIGHT_EXE" cmd commands.list --pretty 2>&1)" || commands_list_exit=$?
    [ "$commands_list_exit" -eq 0 ] \
        || die "commands.list failed on the light build (exit $commands_list_exit): $commands_list_output"
    command_count="$(SMOKE_COMMANDS_LIST="$commands_list_output" "$PYTHON" - <<'PY'
import json, os, sys
envelope = json.loads(os.environ["SMOKE_COMMANDS_LIST"])
if not envelope.get("ok"):
    sys.exit("commands.list envelope reported ok=false on the light build.")
data = envelope.get("data") or {}
commands = data.get("commands") or []
names = [c.get("name", "") if isinstance(c, dict) else str(c) for c in commands]
leaked = [n for n in names if n.startswith("ai.")]
if leaked:
    sys.exit("Light build exposes ai.* commands: %s" % ", ".join(sorted(leaked)))
print(len(names))
PY
)"
    ai_exit=0
    "$LIGHT_EXE" cmd ai.send-once >/dev/null 2>&1 || ai_exit=$?
    [ "$ai_exit" -eq 2 ] \
        || die "ai.send-once should exit 2 (unknown command) on the light build, got $ai_exit."
    LIGHT_CLI_JSON="$("$PYTHON" -c 'import json,sys; print(json.dumps({"CommandsListExitCode": 0, "AiSendOnceExitCode": 2, "CommandCount": int(sys.argv[1])}))' "$command_count")"
fi

new_clean_directory "$PLUGIN_DIR"
expand_archive "$PLUGIN_ZIP" "$PLUGIN_DIR"
UPLUGIN="$PLUGIN_DIR/SmatchetImGuiPlugin/SmatchetImGuiPlugin.uplugin"
[ -f "$UPLUGIN" ] || die "Plugin zip does not contain SmatchetImGuiPlugin.uplugin: $PLUGIN_ZIP"
PLUGIN_VERSION_NAME="$(json_field "$UPLUGIN" VersionName)"
PLUGIN_VERSION_NUMBER="$(json_field "$UPLUGIN" Version)"
if [ -n "${EXPECTED_VERSION//[[:space:]]/}" ] && [ "$PLUGIN_VERSION_NAME" != "$EXPECTED_VERSION" ]; then
    die "Plugin VersionName mismatch. Expected '$EXPECTED_VERSION' but found '$PLUGIN_VERSION_NAME'."
fi

new_clean_directory "$FAB_DIR"
expand_archive "$FAB_ZIP" "$FAB_DIR"
FAB_JSON="$FAB_DIR/fab-submission.json"
[ -f "$FAB_JSON" ] || die "Fab submission zip does not contain fab-submission.json: $FAB_ZIP"
FAB_VERSION="$(json_field "$FAB_JSON" version)"
FAB_TAG="$(json_field "$FAB_JSON" tag)"
if [ -n "${EXPECTED_VERSION//[[:space:]]/}" ] && [ "$FAB_VERSION" != "$EXPECTED_VERSION" ]; then
    die "Fab submission version mismatch. Expected '$EXPECTED_VERSION' but found '$FAB_VERSION'."
fi

new_clean_directory "$SOURCE_DIR"
expand_archive "$SOURCE_ZIP" "$SOURCE_DIR"
source_top_count="$(find "$SOURCE_DIR" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ')"
[ "$source_top_count" -eq 1 ] \
    || die "Source zip should expand to exactly one top-level directory, found $source_top_count."
SOURCE_TREE="$(find "$SOURCE_DIR" -mindepth 1 -maxdepth 1 -type d | head -1)"
for excluded in .claude backlog; do
    if [ -e "$SOURCE_TREE/$excluded" ]; then
        die "Source zip should not contain '$excluded': $SOURCE_ZIP"
    fi
done

installer_args=(--installer-path "$INSTALLER" --expected-version "$EXPECTED_VERSION")
if [ "$CHECK_INSTALLER_SIGNATURES" -eq 1 ]; then
    installer_args+=(--check-signatures)
fi
if [ "$CLEAR_USER_DATA_BEFORE_INSTALL" -eq 1 ]; then
    installer_args+=(--clear-user-data-before-install)
fi
if [ "$KEEP_USER_DATA_AFTER_TEST" -eq 1 ]; then
    installer_args+=(--keep-user-data-after-test)
fi
INSTALLER_SMOKE_JSON="$(bash "$SCRIPT_DIR/test-installer-smoke.sh" "${installer_args[@]}")"

export SMOKE_RELEASE_DIR="$RELEASE_ROOT"
export SMOKE_PORTABLE_VERSION="$PORTABLE_VERSION_JSON"
export SMOKE_LIGHT_VERSION="$LIGHT_VERSION_JSON"
export SMOKE_LIGHT_CLI="$LIGHT_CLI_JSON"
export SMOKE_PLUGIN_VERSION="$PLUGIN_VERSION_NAME"
export SMOKE_PLUGIN_VERSION_NUMBER="$PLUGIN_VERSION_NUMBER"
export SMOKE_FAB_VERSION="$FAB_VERSION"
export SMOKE_FAB_TAG="$FAB_TAG"
export SMOKE_INSTALLER_SMOKE="$INSTALLER_SMOKE_JSON"

"$PYTHON" - <<'PY'
import json, os

summary = {
    "ReleaseDir": os.environ["SMOKE_RELEASE_DIR"],
    "PortableVersionInfo": json.loads(os.environ["SMOKE_PORTABLE_VERSION"]),
    "LightPortableVersionInfo": json.loads(os.environ["SMOKE_LIGHT_VERSION"]),
    "LightCliSmoke": json.loads(os.environ["SMOKE_LIGHT_CLI"]),
    "PluginVersion": os.environ["SMOKE_PLUGIN_VERSION"],
    "PluginVersionNumber": os.environ["SMOKE_PLUGIN_VERSION_NUMBER"],
    "FabSubmissionVersion": os.environ["SMOKE_FAB_VERSION"],
    "FabSubmissionTag": os.environ["SMOKE_FAB_TAG"],
    "InstallerSmoke": json.loads(os.environ["SMOKE_INSTALLER_SMOKE"]),
}
print(json.dumps(summary, indent=2))
PY
