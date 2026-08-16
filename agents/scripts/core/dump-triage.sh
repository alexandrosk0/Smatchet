#!/usr/bin/env bash
# dump-triage.sh — triage a Windows minidump (.dmp) with no native debugger.
#
# Thin bash wrapper over minidump-triage.py (sibling), the dependency-free
# minidump-stream scanner: it extracts the EXCEPTION RECORD (code + faulting
# address + crashing thread) and the MODULE LIST so a faulting address can be
# attributed to a module — the hand-rolled scan a debug-detective previously
# re-derived per incident (tooling backlog no-cdb-windbg-dump-triage-script).
#
# For a SYMBOLIZED stack, install a debugger and use cdb instead:
#     cdb -z <dump.dmp> -c "!analyze -v; q"
# cdb is NOT on a stock dev box and `winget install Microsoft.WinDbg` does not
# provide it (that is the MSIX Store app, no console driver) — the verified
# install recipe and the full canonical invocation both live in
# docs/agent-rules/debug-techniques.md § Installing cdb.
# This wrapper is the no-debugger fallback — fast, lossy (no symbol resolution).
#
# Usage:
#   dump-triage.sh <dump.dmp>     triage one dump (text report on stdout)
#   dump-triage.sh                with no arg, scans %LOCALAPPDATA%\CrashDumps
#                                 for the newest *.dmp and triages it
#   dump-triage.sh --selftest     run the python scanner's selftest fixture
#
# Exit: 0 ok · 1 parse/selftest failure · 2 python missing / no dump found.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY_SCAN="$SCRIPT_DIR/minidump-triage.py"

# Resolve a working python (the Store-alias stub passes command -v but exits 49).
PY=""
for _c in python3 python py; do
    _p="$(command -v "$_c" 2>/dev/null)" || continue
    if "$_p" -c "" >/dev/null 2>&1; then PY="$_p"; break; fi
done
if [ -z "$PY" ]; then
    echo "dump-triage: no working python on PATH (need python3/python/py)" >&2
    exit 2
fi

if [ ! -f "$PY_SCAN" ]; then
    echo "dump-triage: scanner not found: $PY_SCAN" >&2
    exit 2
fi

# --selftest: delegate to the python scanner's fixture-based selftest.
# selftest: asserts-failure: n/a — pure dispatcher; the negative assertion (bad
# dump -> rc 1) lives in minidump-triage.py's selftest, which this delegates to.
if [ "${1:-}" = "--selftest" ]; then
    "$PY" "$PY_SCAN" --selftest
    exit $?
fi

DUMP="${1:-}"

# No arg → newest *.dmp under %LOCALAPPDATA%\CrashDumps (the procdump default).
if [ -z "$DUMP" ]; then
    DUMP_DIR=""
    if [ -n "${LOCALAPPDATA:-}" ]; then
        DUMP_DIR="${LOCALAPPDATA//\\//}/CrashDumps"
    fi
    if [ -z "$DUMP_DIR" ] || [ ! -d "$DUMP_DIR" ]; then
        echo "dump-triage: no dump given and no %LOCALAPPDATA%\\CrashDumps to scan." >&2
        echo "  Usage: dump-triage.sh <dump.dmp>   (or --selftest)" >&2
        exit 2
    fi
    # Newest by mtime; null-safe if the dir has no dumps. ls -t is intentional
    # (mtime sort); crash-dump names are procdump-generated (no spaces/newlines).
    # shellcheck disable=SC2012
    DUMP="$(ls -t "$DUMP_DIR"/*.dmp 2>/dev/null | head -n1 || true)"
    if [ -z "$DUMP" ]; then
        echo "dump-triage: no *.dmp found under $DUMP_DIR" >&2
        exit 2
    fi
    echo "dump-triage: newest dump = $DUMP" >&2
fi

if [ ! -f "$DUMP" ]; then
    echo "dump-triage: file not found: $DUMP" >&2
    exit 2
fi

"$PY" "$PY_SCAN" "$DUMP"
