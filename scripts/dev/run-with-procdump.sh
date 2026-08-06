#!/usr/bin/env bash
# scripts/dev/run-with-procdump.sh — launch Smatchet (or any exe) under Sysinternals
# procdump so a crash that bypasses Windows Error Reporting is still captured as a
# full minidump.
#
# Bash port of the retired run-with-procdump.ps1. Windows-only in effect (procdump
# is a Windows tool) but runs from git-bash like the rest of the toolchain.
#
# Some Smatchet crashes manifest as an "instant vanish" with NO WER dump: the frame-loop
# SEH filter (Source/Standalone/SmatchetCrashHandler.cpp) catches the fault and calls
# std::exit(), which is a deliberate ExitProcess — never an "unhandled" exception WER can
# see. Running the target as a child of procdump captures it anyway: -e dumps on an
# unhandled exception, -t dumps on (abnormal) termination, -ma writes a full dump.
#
# Pair with first-chance breaking under cdb when you need the PRIMARY faulting stack (the
# app's own SEH filter swallows the access violation before WER/procdump's -e sees it):
#   cdb -y "<build-dir>;srv*C:\Tools\symcache*https://msdl.microsoft.com/download/symbols" \
#       -cf scripts/dev/av-capture.cdb <exe>
# where av-capture.cdb does: sxe -c "kP; .dump /ma <out.dmp>; q" av ; g
#
# procdump itself: https://download.sysinternals.com/files/Procdump.zip
# Machine-wide auto-capture for EVERY future crash (one-time, elevated):
#   procdump64.exe -accepteula -ma -i <dump-dir>
#
# Examples:
#   bash scripts/dev/run-with-procdump.sh
#   bash scripts/dev/run-with-procdump.sh --autocycle-panes 20

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

EXE="$REPO_ROOT/build/ninja-iter-msvc/Smatchet.exe"
DUMP_DIR="${LOCALAPPDATA:-$HOME}/CrashDumps"
PROCDUMP="C:/Tools/Procdump/procdump64.exe"
AUTOCYCLE_PANES=0

usage() {
    cat <<'EOF'
Usage: bash scripts/dev/run-with-procdump.sh [options]

Options:
  --exe <path>               Executable to launch (default: build/ninja-iter-msvc/Smatchet.exe)
  --dump-dir <path>          Where procdump writes dumps (default: %LOCALAPPDATA%/CrashDumps)
  --procdump <path>          procdump64.exe location (default: C:/Tools/Procdump/procdump64.exe)
  --autocycle-panes <frames> Export SMATCHET_AUTOCYCLE_PANES so a debug build auto-cycles the
                             focused grid pane every N frames (hands-free repro of the
                             rapid-backend-switch crash class). Requires a build honouring it.
  -h, --help                 Show this help
EOF
}

# `shift 2` fails WITHOUT shifting when the option value is missing, which
# would spin this loop forever (no `set -e` here). Reject that up front.
need_value() {
    [ "$1" -ge 2 ] || { echo "run-with-procdump: $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --exe)                 need_value $# "$1"; EXE="$2"; shift 2 ;;
        --exe=*)               EXE="${1#*=}"; shift ;;
        --dump-dir)            need_value $# "$1"; DUMP_DIR="$2"; shift 2 ;;
        --dump-dir=*)          DUMP_DIR="${1#*=}"; shift ;;
        --procdump)            need_value $# "$1"; PROCDUMP="$2"; shift 2 ;;
        --procdump=*)          PROCDUMP="${1#*=}"; shift ;;
        --autocycle-panes)     need_value $# "$1"; AUTOCYCLE_PANES="$2"; shift 2 ;;
        --autocycle-panes=*)   AUTOCYCLE_PANES="${1#*=}"; shift ;;
        -h|--help)             usage; exit 0 ;;
        *) echo "run-with-procdump: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[ -f "$EXE" ] || { echo "run-with-procdump: executable not found: $EXE" >&2; exit 1; }
[ -f "$PROCDUMP" ] || {
    echo "run-with-procdump: procdump not found at $PROCDUMP. Get it: https://download.sysinternals.com/files/Procdump.zip (extract procdump64.exe to C:\\Tools\\Procdump)." >&2
    exit 1
}
mkdir -p "$DUMP_DIR" || { echo "run-with-procdump: cannot create dump dir: $DUMP_DIR" >&2; exit 1; }

EXE_FULL="$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")"

if [ "$AUTOCYCLE_PANES" -gt 0 ] 2>/dev/null; then
    export SMATCHET_AUTOCYCLE_PANES="$AUTOCYCLE_PANES"
fi

# -ma full dump, -e on unhandled exception, -t on termination, -x launches + monitors.
echo "Launching under procdump:"
echo "  $PROCDUMP -accepteula -ma -e -t -x $DUMP_DIR $EXE_FULL"
echo "Dumps -> $DUMP_DIR"
exec "$PROCDUMP" -accepteula -ma -e -t -x "$DUMP_DIR" "$EXE_FULL"
