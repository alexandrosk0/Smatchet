#!/usr/bin/env bash
# is-exe-fresh.sh — warn if a built Smatchet.exe is OLDER than its sources (i.e.
# you're about to run/test a stale exe because a rebuild was skipped or a no-op).
# Mirrors the WarnIfPackagedLibsAreStale mtime-compare in
# Source/UnrealPlugins/SmatchetImGuiPlugin/.../SmatchetImGuiPlugin.Build.cs, but
# for the standalone exe. build-quality-velocity-hardening #6.
#
# Wired into scripts/dev/relaunch-smatchet.sh (post-build, pre-launch) so a
# build that was a no-op while the running process held the exe lock is caught
# before the stale exe is relaunched. Also runnable standalone before any
# manual / scenario run of an exe you did not just rebuild.
#
# Usage:
#   bash scripts/dev/is-exe-fresh.sh [--preset <preset>] [--exe <path>]
#   bash scripts/dev/is-exe-fresh.sh --selftest
#
# Default preset ninja-iter-msvc; exe build/<preset>/Smatchet.exe.
#
# Exit: 0 fresh (or indeterminate — missing exe/sources never hard-fails a run
#       path); 3 STALE (a Source/ file is newer than the exe); 1 selftest fail.

set -uo pipefail

_file_mtime() { stat -c '%Y' "$1" 2>/dev/null || stat -f '%m' "$1" 2>/dev/null; }

# Newest first-party source mtime (epoch seconds) under <dir>. Empty if none.
# Loops via _file_mtime (portable: GNU `stat -c` + BSD `stat -f`) instead of
# GNU-only `find -printf`, so it works on macOS/BSD too (CR #915). The while body
# runs in the current shell (process substitution), so `newest` persists.
_newest_source_mtime() {
    local f newest=0 m
    while IFS= read -r f; do
        m="$(_file_mtime "$f")"
        [ -n "$m" ] && [ "$m" -gt "$newest" ] && newest="$m"
    done < <(find "$1" -type f \
        \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.c' \
           -o -name '*.cc' -o -name '*.cxx' -o -name '*.inl' \) 2>/dev/null)
    [ "$newest" -gt 0 ] && printf '%s\n' "$newest"
}

# is_stale <exe> <source-dir> -> echoes "stale"/"fresh"/"indeterminate"
_freshness() {
    local exe="$1" srcdir="$2"
    [ -f "$exe" ] || { echo indeterminate; return; }
    [ -d "$srcdir" ] || { echo indeterminate; return; }
    local newest exe_m
    newest="$(_newest_source_mtime "$srcdir")"
    exe_m="$(_file_mtime "$exe")"
    [ -n "$newest" ] && [ -n "$exe_m" ] || { echo indeterminate; return; }
    if [ "$newest" -gt "$exe_m" ]; then echo stale; else echo fresh; fi
}

# --- selftest ---------------------------------------------------------------
if [ "${1:-}" = "--selftest" ]; then
    fail=0; tmp="$(mktemp -d)" || { echo "is-exe-fresh selftest: mktemp failed" >&2; exit 1; }
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/Source"
    : > "$tmp/Source/a.cpp"
    exe="$tmp/app.exe"; : > "$exe"
    # exe newer than the source (just touched after) -> fresh
    touch "$exe"
    [ "$(_freshness "$exe" "$tmp/Source")" = fresh ] || { echo "FAIL: expected fresh"; fail=1; }
    # make a source newer than the exe -> stale
    sleep 1; : > "$tmp/Source/b.cpp"
    [ "$(_freshness "$exe" "$tmp/Source")" = stale ] || { echo "FAIL: expected stale"; fail=1; }
    # missing exe -> indeterminate (never hard-fails)
    [ "$(_freshness "$tmp/nope.exe" "$tmp/Source")" = indeterminate ] || { echo "FAIL: expected indeterminate"; fail=1; }
    [ "$fail" = 0 ] && { echo "is-exe-fresh --selftest: PASS"; exit 0; } || { echo "is-exe-fresh --selftest: FAIL"; exit 1; }
fi

cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 0
PRESET="ninja-iter-msvc"
EXE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) PRESET="$2"; shift 2 ;;
        --preset=*) PRESET="${1#--preset=}"; shift ;;
        --exe) EXE="$2"; shift 2 ;;
        --exe=*) EXE="${1#--exe=}"; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "is-exe-fresh: unknown arg: $1" >&2; exit 0 ;;
    esac
done
[ -n "$EXE" ] || EXE="build/$PRESET/Smatchet.exe"

case "$(_freshness "$EXE" Source)" in
    stale)
        echo "is-exe-fresh: WARN — $EXE is STALE (a Source/ file is newer than the exe)." >&2
        echo "  Rebuild before running: bash scripts/dev/with-msvc-env.sh cmake --build --preset $PRESET --target SmatchetStandalone" >&2
        exit 3 ;;
    indeterminate)
        echo "is-exe-fresh: indeterminate ($EXE or Source/ not found) — skipping." >&2
        exit 0 ;;
    *)
        echo "is-exe-fresh: $EXE is fresh (newer than every Source/ file)." >&2
        exit 0 ;;
esac
