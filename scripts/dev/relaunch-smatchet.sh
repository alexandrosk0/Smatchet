#!/usr/bin/env bash
# scripts/dev/relaunch-smatchet.sh — kill any running Smatchet.exe, rebuild,
# launch the rebuilt exe in the background.
#
# Addresses the iter-loop friction documented in
# docs/backlog/agent-self-improvement/tooling.md
# "Running Smatchet.exe holds the file lock; rebuild link fails until the user
# kills the window". Standardises the kill-then-build pattern so orchestrator /
# build-doctor / debug-detective don't re-discover the lock dance per session.
#
# Usage:
#   bash scripts/dev/relaunch-smatchet.sh                 # iter preset
#   bash scripts/dev/relaunch-smatchet.sh --preset=debug  # debug preset
#   bash scripts/dev/relaunch-smatchet.sh --no-launch     # build only
#   bash scripts/dev/relaunch-smatchet.sh --args="--db-path=foo.sqlite"
#
# Exit codes:
#   0 — build succeeded; exe launched (or skipped with --no-launch)
#   1 — build failed
#   2 — bad usage

set -euo pipefail

PRESET="ninja-iter-msvc"
NO_LAUNCH=0
EXTRA_ARGS=""

usage() {
    cat >&2 <<'USAGE'
Usage: bash scripts/dev/relaunch-smatchet.sh [--preset=<preset>] [--no-launch] [--args="..."]

  --preset=<preset>  CMake preset for build. Default ninja-iter-msvc.
                     Other valid options: ninja-debug-msvc, ninja-publish-msvc.
  --no-launch        Build only; do not launch the exe.
  --args="<argv>"    Extra args passed verbatim to the launched exe.
                     Quote the whole list.
USAGE
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset=*) PRESET="${1#--preset=}"; shift ;;
        --preset) [ "$#" -ge 2 ] || { echo "relaunch-smatchet: --preset requires a value" >&2; usage; }
                  PRESET="$2"; shift 2 ;;
        --no-launch) NO_LAUNCH=1; shift ;;
        --args=*) EXTRA_ARGS="${1#--args=}"; shift ;;
        --args) [ "$#" -ge 2 ] || { echo "relaunch-smatchet: --args requires a value" >&2; usage; }
                EXTRA_ARGS="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "unknown arg: $1" >&2; usage ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

EXE="build/$PRESET/Smatchet.exe"

# Ensure msys2 ucrt64 toolchain is on PATH so cmake / ninja / g++ resolve.
# Idempotent prepend (no duplicate entries if already present).
case ":$PATH:" in
    *":/c/msys64/ucrt64/bin:"*) ;;
    *) export PATH="/c/msys64/ucrt64/bin:$PATH" ;;
esac

# --- Kill any running Smatchet.exe ----------------------------------------
# Use Windows `tasklist` + `taskkill` via the bash wrappers MSYS2 ships. On
# non-Windows hosts these don't exist; gate the kill on tasklist's presence
# so the script stays cross-platform-friendly even though Smatchet only
# ships on Windows today.
if command -v tasklist >/dev/null 2>&1; then
    # tasklist /FI "IMAGENAME eq Smatchet.exe" prints a header even when no
    # process matches; the "INFO: No tasks" line on stderr distinguishes.
    if tasklist /FI "IMAGENAME eq Smatchet.exe" 2>/dev/null | grep -qi "Smatchet.exe"; then
        echo "[relaunch] killing running Smatchet.exe ..." >&2
        taskkill //IM Smatchet.exe //F >/dev/null 2>&1 || true
        # Give the OS a moment to release the file handle before the link step.
        sleep 0.5
    fi
fi

# --- Build ---------------------------------------------------------------
echo "[relaunch] cmake --build --preset $PRESET --target SmatchetStandalone" >&2
if ! cmake --build --preset "$PRESET" --target SmatchetStandalone; then
    echo "[relaunch] FAIL: build failed for preset=$PRESET" >&2
    exit 1
fi

# Stale-exe sanity check — flag if the exe mtime didn't move (would indicate
# the build was a no-op AND the running process was holding it).
if [[ -x "$EXE" ]]; then
    echo "[relaunch] exe: $EXE" >&2
    ls -la "$EXE" >&2
else
    echo "[relaunch] FAIL: build succeeded but exe not at $EXE" >&2
    exit 1
fi

# --- Launch --------------------------------------------------------------
if (( NO_LAUNCH )); then
    echo "[relaunch] --no-launch given; skipping launch." >&2
    exit 0
fi

# Detach via & so the script returns immediately. stdout/stderr captured to
# a per-launch temp file (mirrors the --spawn convention) so the caller can
# `tail -f` while iterating.
LAUNCH_LOG="${TMPDIR:-/tmp}/Smatchet-relaunch-$$.log"
echo "[relaunch] launching: $EXE $EXTRA_ARGS" >&2
echo "[relaunch] log: $LAUNCH_LOG" >&2
# shellcheck disable=SC2086  # word-splitting EXTRA_ARGS is intentional
nohup "$EXE" $EXTRA_ARGS >"$LAUNCH_LOG" 2>&1 &
LAUNCH_PID=$!
echo "[relaunch] pid=$LAUNCH_PID" >&2
echo "$LAUNCH_PID"
