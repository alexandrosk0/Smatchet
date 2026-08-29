#!/usr/bin/env bash
# scripts/publish/test-installer-smoke.sh — install/launch/uninstall smoke test
# for the packaged Windows installer.
#
# Bash port of the retired test_installer_smoke.ps1. Windows-only (Inno per-user
# installer + HKCU uninstall key), run from Git Bash.
#
# Round-trip: silent-uninstall any leftover install -> install silently -> assert
# the exe, uninstaller, and uninstall registry key exist -> validate the embedded
# version resource -> launch briefly -> silent-uninstall -> assert the install dir
# and registry key are gone. Prints a JSON summary on success.
#
# Protocol described in scripts/publish/INSTALLER_SMOKE_TEST.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

INSTALLER_PATH=""
RELEASE_DIR=""
EXPECTED_VERSION=""
LAUNCH_SECONDS=5
CHECK_SIGNATURES=0
CLEAR_USER_DATA_BEFORE_INSTALL=0
KEEP_USER_DATA_AFTER_TEST=0

usage() {
    cat <<'EOF'
Usage: bash scripts/publish/test-installer-smoke.sh (--installer-path <exe> | --release-dir <dir>) [options]

  --installer-path <exe>   Installer to test
  --release-dir <dir>      Release root; picks the newest assets/*windows-setup.exe
  --expected-version <ver> Assert the exe's FileVersion/ProductVersion
  --launch-seconds <n>     Seconds to leave the app running (default: 5)
  --check-signatures       Record Authenticode status for installer/exe/uninstaller
  --clear-user-data-before-install  Delete %LOCALAPPDATA%\Smatchet first
  --keep-user-data-after-test       Leave user data in place when done
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
        --installer-path)    need_value $# "$1"; INSTALLER_PATH="$2"; shift 2 ;;
        --installer-path=*)  INSTALLER_PATH="${1#*=}"; shift ;;
        --release-dir)       need_value $# "$1"; RELEASE_DIR="$2"; shift 2 ;;
        --release-dir=*)     RELEASE_DIR="${1#*=}"; shift ;;
        --expected-version)  need_value $# "$1"; EXPECTED_VERSION="$2"; shift 2 ;;
        --expected-version=*) EXPECTED_VERSION="${1#*=}"; shift ;;
        --launch-seconds)    need_value $# "$1"; LAUNCH_SECONDS="$2"; shift 2 ;;
        --launch-seconds=*)  LAUNCH_SECONDS="${1#*=}"; shift ;;
        --check-signatures)  CHECK_SIGNATURES=1; shift ;;
        --clear-user-data-before-install) CLEAR_USER_DATA_BEFORE_INSTALL=1; shift ;;
        --keep-user-data-after-test)      KEEP_USER_DATA_AFTER_TEST=1; shift ;;
        -h|--help)           usage; exit 0 ;;
        *)
            echo "test-installer-smoke: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

die() { echo "test-installer-smoke: $*" >&2; exit 1; }

# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/agents/scripts/core/lib/resolve-py.sh"
PYTHON="$(resolve_py)" || PYTHON=""
command -v "$PYTHON" >/dev/null 2>&1 || die "python is required."

abs_path() { "$PYTHON" -c 'import os,sys; print(os.path.abspath(sys.argv[1]))' "$1"; }
winpath() { command -v cygpath >/dev/null 2>&1 && cygpath -w "$1" || printf '%s\n' "$1"; }

resolve_installer_path() {
    local full assets match
    if [ -n "${INSTALLER_PATH//[[:space:]]/}" ]; then
        full="$(abs_path "$INSTALLER_PATH")"
        [ -f "$full" ] || die "Installer not found: $full"
        printf '%s\n' "$full"
        return 0
    fi
    [ -n "${RELEASE_DIR//[[:space:]]/}" ] || die "Specify either --installer-path or --release-dir."
    assets="$(abs_path "$RELEASE_DIR")/assets"
    # Arch-agnostic: ARM64 assets carry an "-arm64" token between "windows" and
    # "setup", so an exact '*windows-setup.exe' glob misses them entirely.
    match="$(find "$assets" -maxdepth 1 -type f -name '*windows*setup.exe' -printf '%T@\t%p\n' 2>/dev/null \
        | sort -rn | head -1 | cut -f2- || true)"
    [ -n "$match" ] || die "Could not find *windows*setup.exe under $assets"
    printf '%s\n' "$match"
}

UNINSTALL_KEY='HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\{6A63A3FA-86B2-4574-B0F6-7C8781B10C0F}_is1'

uninstall_key_present() {
    reg query "$UNINSTALL_KEY" >/dev/null 2>&1
}

invoke_silent_uninstall_if_present() {
    local uninstall_string
    uninstall_key_present || return 0
    uninstall_string="$(reg query "$UNINSTALL_KEY" /v UninstallString 2>/dev/null \
        | sed -n 's/.*UninstallString[[:space:]]*REG_[A-Z_]*[[:space:]]*//p' | head -1)"
    uninstall_string="${uninstall_string%$'\r'}"
    uninstall_string="${uninstall_string%\"}"
    uninstall_string="${uninstall_string#\"}"
    [ -n "${uninstall_string//[[:space:]]/}" ] || return 0
    local uninstaller
    uninstaller="$(cygpath -u "$uninstall_string" 2>/dev/null || printf '%s' "$uninstall_string")"
    MSYS_NO_PATHCONV=1 "$uninstaller" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART || true
    # Inno's uninstaller relaunches itself from a temp copy and returns before
    # the real work finishes, so wait for the registry key to disappear rather
    # than trusting the exit of the first process.
    local waited=0
    while uninstall_key_present && [ "$waited" -lt 60 ]; do
        sleep 1
        waited=$((waited + 1))
    done
}

# Authenticode summary via signtool (the bash analogue of Get-AuthenticodeSignature).
# Absent signtool or an unsigned file yields Status=Unknown rather than failing the
# run — signature checking is opt-in reporting, not a gate here.
signature_summary_json() {
    local path="$1" out status subject thumbprint
    if ! command -v signtool >/dev/null 2>&1 && ! command -v signtool.exe >/dev/null 2>&1; then
        "$PYTHON" -c 'import json,sys; print(json.dumps({"Path": sys.argv[1], "Status": "Unknown", "StatusMessage": "signtool.exe not found on PATH", "Subject": "", "Thumbprint": ""}))' "$path"
        return 0
    fi
    if out="$(MSYS_NO_PATHCONV=1 signtool verify /pa /v "$(winpath "$path")" 2>&1)"; then
        status="Valid"
    elif printf '%s' "$out" | grep -qi 'No signature found'; then
        status="NotSigned"
    else
        # signtool failed for a non-Authenticode reason (bad invocation, I/O):
        # report Unknown rather than a false NotSigned verdict on a signed file.
        status="Unknown"
    fi
    subject="$(printf '%s' "$out" | sed -n 's/.*Issued to:[[:space:]]*//p' | head -1)"
    thumbprint="$(printf '%s' "$out" | sed -n 's/.*SHA1 hash:[[:space:]]*//p' | head -1 | tr -d '\r')"
    "$PYTHON" -c '
import json, sys
print(json.dumps({
    "Path": sys.argv[1], "Status": sys.argv[2], "StatusMessage": sys.argv[3].strip(),
    "Subject": sys.argv[4].strip(), "Thumbprint": sys.argv[5].strip(),
}))' "$path" "$status" "$out" "$subject" "$thumbprint"
}

INSTALLER="$(resolve_installer_path)"
INSTALL_DIR="${LOCALAPPDATA:?LOCALAPPDATA is not set}/Programs/Smatchet"
USER_DATA_DIR="$LOCALAPPDATA/Smatchet"
START_MENU_LINK="${APPDATA:?APPDATA is not set}/Microsoft/Windows/Start Menu/Programs/Smatchet.lnk"

invoke_silent_uninstall_if_present
if [ -d "$INSTALL_DIR" ]; then
    rm -rf "$INSTALL_DIR"
fi
if [ "$CLEAR_USER_DATA_BEFORE_INSTALL" -eq 1 ] && [ -d "$USER_DATA_DIR" ]; then
    rm -rf "$USER_DATA_DIR"
fi

INSTALLER_SIGNATURE="null"
if [ "$CHECK_SIGNATURES" -eq 1 ]; then
    INSTALLER_SIGNATURE="$(signature_summary_json "$INSTALLER")"
fi

MSYS_NO_PATHCONV=1 "$INSTALLER" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART || die "Installer exited non-zero."

INSTALLED_EXE="$INSTALL_DIR/Smatchet.exe"
INSTALLED_UNINSTALLER="$INSTALL_DIR/unins000.exe"
START_MENU_LINK_EXISTS=false
if [ -f "$START_MENU_LINK" ]; then
    START_MENU_LINK_EXISTS=true
fi

[ -f "$INSTALLED_EXE" ] || die "Installed exe missing: $INSTALLED_EXE"
[ -f "$INSTALLED_UNINSTALLER" ] || die "Installed uninstaller missing: $INSTALLED_UNINSTALLER"
uninstall_key_present || die "Uninstall registry key missing after install: $UNINSTALL_KEY"

INSTALLED_VERSION_JSON="$("$PYTHON" "$SCRIPT_DIR/test-windows-version-info.py" \
    "$INSTALLED_EXE" --expected-version "$EXPECTED_VERSION")" \
    || die "Installed exe version-info validation failed."

EXE_SIGNATURE="null"
UNINSTALLER_SIGNATURE="null"
if [ "$CHECK_SIGNATURES" -eq 1 ]; then
    EXE_SIGNATURE="$(signature_summary_json "$INSTALLED_EXE")"
    UNINSTALLER_SIGNATURE="$(signature_summary_json "$INSTALLED_UNINSTALLER")"
fi

"$INSTALLED_EXE" >/dev/null 2>&1 &
APP_PID=$!
sleep "$LAUNCH_SECONDS"
# Liveness is the assertion, not just a kill precondition: an exe that dies on
# startup would otherwise sail through this step as a pass.
kill -0 "$APP_PID" 2>/dev/null \
    || die "Installed exe exited within ${LAUNCH_SECONDS}s of launch: $INSTALLED_EXE"
kill -9 "$APP_PID" 2>/dev/null || taskkill //F //PID "$APP_PID" >/dev/null 2>&1 || true
wait "$APP_PID" 2>/dev/null || true
sleep 1

USER_DATA_AFTER_LAUNCH=""
if [ -d "$USER_DATA_DIR" ]; then
    USER_DATA_AFTER_LAUNCH="$(find "$USER_DATA_DIR" -type f | sort)"
fi

invoke_silent_uninstall_if_present
sleep 1

if [ -d "$INSTALL_DIR" ]; then
    die "Install directory still exists after uninstall: $INSTALL_DIR"
fi
if uninstall_key_present; then
    die "Uninstall registry key still exists after uninstall: $UNINSTALL_KEY"
fi

USER_DATA_AFTER_UNINSTALL=""
if [ -d "$USER_DATA_DIR" ]; then
    USER_DATA_AFTER_UNINSTALL="$(find "$USER_DATA_DIR" -type f | sort)"
    if [ "$KEEP_USER_DATA_AFTER_TEST" -eq 0 ]; then
        rm -rf "$USER_DATA_DIR"
    fi
fi

export SMOKE_INSTALLER="$INSTALLER"
export SMOKE_START_MENU="$START_MENU_LINK_EXISTS"
export SMOKE_VERSION_JSON="$INSTALLED_VERSION_JSON"
export SMOKE_INSTALLER_SIG="$INSTALLER_SIGNATURE"
export SMOKE_EXE_SIG="$EXE_SIGNATURE"
export SMOKE_UNINST_SIG="$UNINSTALLER_SIGNATURE"
export SMOKE_USER_DATA_DIR="$USER_DATA_DIR"
export SMOKE_USER_DATA_LAUNCH="$USER_DATA_AFTER_LAUNCH"
export SMOKE_USER_DATA_UNINSTALL="$USER_DATA_AFTER_UNINSTALL"

"$PYTHON" - <<'PY'
import json, os

def lines(value):
    return [line for line in value.splitlines() if line.strip()]

summary = {
    "InstallerPath": os.environ["SMOKE_INSTALLER"],
    "StartMenuLinkExistsAfterInstall": os.environ["SMOKE_START_MENU"] == "true",
    "InstalledVersionInfo": json.loads(os.environ["SMOKE_VERSION_JSON"]),
    "InstallerSignature": json.loads(os.environ["SMOKE_INSTALLER_SIG"]),
    "InstalledExeSignature": json.loads(os.environ["SMOKE_EXE_SIG"]),
    "InstalledUninstallerSignature": json.loads(os.environ["SMOKE_UNINST_SIG"]),
    "UserDataDirectory": os.environ["SMOKE_USER_DATA_DIR"],
    "UserDataFilesAfterLaunch": lines(os.environ["SMOKE_USER_DATA_LAUNCH"]),
    "UserDataFilesAfterUninstall": lines(os.environ["SMOKE_USER_DATA_UNINSTALL"]),
    "InstallDirRemovedAfterUninstall": True,
}
print(json.dumps(summary, indent=2))
PY
