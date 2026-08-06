#!/usr/bin/env bash
# scripts/dev/local/attach-unreal-vsjit.sh — attach the VS JIT debugger to a
# running UnrealEditor.
#
# Bash port of the retired attach_unreal_vsjit.ps1. Windows-only by nature (it
# drives vsjitdebugger.exe), but it needs no PowerShell: tasklist gives the PID
# and vswhere finds the debugger.
#
# Usage:
#   bash scripts/dev/local/attach-unreal-vsjit.sh
#   bash scripts/dev/local/attach-unreal-vsjit.sh --process-name UnrealEditor-Win64-DebugGame

set -uo pipefail

PROCESS_NAME="UnrealEditor"

usage() {
    cat <<'EOF'
Usage: bash scripts/dev/local/attach-unreal-vsjit.sh [options]

  --process-name <name>   Process to attach to (default: UnrealEditor)
  -h, --help              Show this help
EOF
}

# `shift 2` fails WITHOUT shifting when the option value is missing, which
# would spin this loop forever (no `set -e` here). Reject that up front.
need_value() {
    [ "$1" -ge 2 ] || { echo "attach-unreal-vsjit: $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --process-name) need_value $# "$1"; PROCESS_NAME="$2"; shift 2 ;;
        -h|--help)      usage; exit 0 ;;
        *)
            echo "attach-unreal-vsjit: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

die() { echo "attach-unreal-vsjit: $*" >&2; exit 1; }

# --- Locate the process -----------------------------------------------------
command -v tasklist >/dev/null 2>&1 || die "tasklist not found - this script is Windows-only."

# CSV output so the PID column survives names with spaces.
PID_LINE="$(tasklist /FI "IMAGENAME eq ${PROCESS_NAME}.exe" /FO CSV /NH 2>/dev/null | head -1)"
case "$PID_LINE" in
    '"'*) ;;
    *) PID_LINE="" ;;
esac
[ -n "$PID_LINE" ] || die "no running process named '$PROCESS_NAME' found. Launch UnrealEditor first (preferably with -WaitForDebugger)."

TARGET_PID="$(printf '%s\n' "$PID_LINE" | awk -F'","' '{print $2}')"
[ -n "$TARGET_PID" ] || die "could not parse a PID out of tasklist output: $PID_LINE"

# --- Locate vsjitdebugger.exe -----------------------------------------------
# `ProgramFiles(x86)` is not a legal shell identifier, so it cannot be read as
# $ProgramFiles(x86) the way the PowerShell original did — go through env.
PROGRAM_FILES_X86="$(env | sed -n 's/^ProgramFiles(x86)=//p' | head -1)"
[ -n "$PROGRAM_FILES_X86" ] && PROGRAM_FILES_X86="$(cygpath -u "$PROGRAM_FILES_X86" 2>/dev/null || printf '%s' "$PROGRAM_FILES_X86")"
[ -n "$PROGRAM_FILES_X86" ] || PROGRAM_FILES_X86="/c/Program Files (x86)"

PROGRAM_FILES="${PROGRAMFILES:-}"
[ -n "$PROGRAM_FILES" ] && PROGRAM_FILES="$(cygpath -u "$PROGRAM_FILES" 2>/dev/null || printf '%s' "$PROGRAM_FILES")"
[ -n "$PROGRAM_FILES" ] || PROGRAM_FILES="/c/Program Files"

find_vsjit_via_vswhere() {
    local vswhere hit
    vswhere="$PROGRAM_FILES_X86/Microsoft Visual Studio/Installer/vswhere.exe"
    [ -f "$vswhere" ] || return 1
    # No -requires: some installs omit the package IDs strict workloads expect
    # yet still ship vsjitdebugger.exe.
    hit="$("$vswhere" -latest -products '*' -prerelease -sort -find '**\vsjitdebugger.exe' 2>/dev/null | head -1 | tr -d '\r')"
    [ -n "$hit" ] || return 1
    printf '%s\n' "$hit"
}

find_vsjit_heuristic() {
    local root year edition explicit hit
    for root in "$PROGRAM_FILES/Microsoft Visual Studio" \
                "$PROGRAM_FILES_X86/Microsoft Visual Studio"; do
        [ -d "$root" ] || continue
        for year in 2022 2026 2019; do
            [ -d "$root/$year" ] || continue
            for edition in Community Professional Enterprise Preview BuildTools; do
                explicit="$root/$year/$edition/Common7/IDE/vsjitdebugger.exe"
                if [ -f "$explicit" ]; then
                    printf '%s\n' "$explicit"
                    return 0
                fi
            done
        done
        # Bounded-depth sweep for nonstandard layouts.
        hit="$(find "$root" -maxdepth 8 -type f -name vsjitdebugger.exe 2>/dev/null | head -1)"
        if [ -n "$hit" ]; then
            printf '%s\n' "$hit"
            return 0
        fi
    done
    return 1
}

VSJIT="$(find_vsjit_via_vswhere || find_vsjit_heuristic)" || VSJIT=""

if [ -z "$VSJIT" ]; then
    cat >&2 <<EOF
attach-unreal-vsjit: could not find vsjitdebugger.exe.

This tool is optional and may be missing even when Visual Studio is installed.

Fix options:
- Visual Studio Installer -> Modify your VS 2022 instance -> Individual components:
  search for "Just-In-Time debugger" and install it, or ensure the
  "Desktop development with C++" workload is fully installed (repair if needed).

Workaround (manual attach):
- Visual Studio -> Debug -> Attach to Process... -> select $PROCESS_NAME (PID $TARGET_PID)

CLI alternative:
- windbg -p $TARGET_PID
EOF
    exit 1
fi

echo "==> Attaching VS JIT debugger"
echo "    Process: $PROCESS_NAME (PID $TARGET_PID)"
echo "    Debugger: $VSJIT"

"$VSJIT" -p "$TARGET_PID" || die "vsjitdebugger returned exit code $?"
