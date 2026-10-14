#!/usr/bin/env bash
# is-exe-fresh.sh — warn if a built Smatchet.exe is OLDER than its sources (i.e.
# you're about to run/test a stale exe because a rebuild was skipped or a no-op).
# Mirrors the WarnIfPackagedLibsAreStale mtime-compare in
# Source/UnrealPlugins/SmatchetImGuiPlugin/.../SmatchetImGuiPlugin.Build.cs, but
# for the standalone exe. build-quality-velocity-hardening #6.
#
# Wired into scripts/dev/relaunch-smatchet.sh (post-build, pre-launch) so a
# build that was a no-op while the running process held the exe lock is caught
# before the stale exe is relaunched, and into the bucket-E driver helper
# (scripts/dev/lib/ui-test-driver.sh, which checks only the trees linked into
# Smatchet.exe and turns STALE into a hard exit 2). Also runnable standalone before any manual /
# scenario run of an exe you did not just rebuild.
#
# Usage:
#   bash scripts/dev/is-exe-fresh.sh [--preset <preset>] [--exe <path>] [--src <dir>]...
#   bash scripts/dev/is-exe-fresh.sh --selftest
#
# Default preset ninja-iter-msvc; exe build/<preset>/Smatchet.exe. --src is
# repeatable and takes a dir or a single source file (a relative path resolves
# from the repo root); default Source/. A fresh tree costs one `find -newer`
# walk, not a stat per source file.
#
# Exit: 0 fresh (or indeterminate — missing exe/sources never hard-fails a run
#       path); 3 STALE (a source file is newer than the exe — the newest offender
#       is named); 1 selftest fail.

set -uo pipefail

_file_mtime() { stat -c '%Y' "$1" 2>/dev/null || stat -f '%m' "$1" 2>/dev/null; }

# First-party C/C++ source names — the files a rebuild would recompile.
_SRC_NAMES=(\( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.c'
            -o -name '*.cc' -o -name '*.cxx' -o -name '*.inl' \))

# _freshness <exe> <source-dir-or-file>... -> echoes "stale <newest-offending-file>" /
# "fresh" / "indeterminate". POSIX `find -newer` (no GNU-only -printf) keeps it
# portable to macOS/BSD (CR #915); only the offending files — usually a handful
# — are stat'ed, to name the newest one. The while body runs in the current
# shell (process substitution), so `newest` persists.
_freshness() {
    local exe="$1" d f m newest=0 newest_f=""
    shift
    local dirs=()
    for d in "$@"; do [ -e "$d" ] && dirs+=("$d"); done
    [ -f "$exe" ] || { echo indeterminate; return; }
    [ "${#dirs[@]}" -gt 0 ] || { echo indeterminate; return; }
    # An empty source tree is indeterminate, not fresh.
    [ -n "$(find "${dirs[@]}" -type f "${_SRC_NAMES[@]}" -print -quit 2>/dev/null)" ] \
        || { echo indeterminate; return; }
    while IFS= read -r f; do
        m="$(_file_mtime "$f")"
        if [ -n "$m" ] && [ "$m" -ge "$newest" ]; then newest="$m"; newest_f="$f"; fi
    done < <(find "${dirs[@]}" -type f "${_SRC_NAMES[@]}" -newer "$exe" 2>/dev/null)
    if [ -n "$newest_f" ]; then echo "stale $newest_f"; else echo fresh; fi
}

# --- selftest ---------------------------------------------------------------
if [ "${1:-}" = "--selftest" ]; then
    fail=0; tmp="$(mktemp -d)" || { echo "is-exe-fresh selftest: mktemp failed" >&2; exit 1; }
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/Source" "$tmp/tests/ui"
    : > "$tmp/Source/a.cpp"
    : > "$tmp/tests/ui/t.test.cpp"
    exe="$tmp/app.exe"; : > "$exe"
    # exe newer than every source (just touched after) -> fresh
    touch "$exe"
    [ "$(_freshness "$exe" "$tmp/Source" "$tmp/tests/ui")" = fresh ] || { echo "FAIL: expected fresh"; fail=1; }
    # make a source newer than the exe -> stale, naming the offender
    # selftest: asserts-failure — a source newer than the exe must be detected as stale (the warn path).
    sleep 1; : > "$tmp/Source/b.cpp"
    r="$(_freshness "$exe" "$tmp/Source")"
    [ "${r%% *}" = stale ] || { echo "FAIL: expected stale, got '$r'"; fail=1; }
    [ "${r#stale }" = "$tmp/Source/b.cpp" ] || { echo "FAIL: expected b.cpp named, got '$r'"; fail=1; }
    # a second source dir is checked too: an exe fresh against dir 1 but older
    # than a dir-2 file is stale, and the newest offender across dirs is named
    touch "$exe"; sleep 1; : > "$tmp/tests/ui/u.test.cpp"
    r="$(_freshness "$exe" "$tmp/Source" "$tmp/tests/ui")"
    [ "$r" = "stale $tmp/tests/ui/u.test.cpp" ] || { echo "FAIL: expected multi-dir stale on u.test.cpp, got '$r'"; fail=1; }
    [ "$(_freshness "$exe" "$tmp/Source")" = fresh ] || { echo "FAIL: expected fresh against Source/ alone"; fail=1; }
    # a single FILE is a source too: stale when it is newer, ignored when it is not a source name
    r="$(_freshness "$exe" "$tmp/Source" "$tmp/tests/ui/u.test.cpp")"
    [ "$r" = "stale $tmp/tests/ui/u.test.cpp" ] || { echo "FAIL: expected a file --src to count, got '$r'"; fail=1; }
    # non-source files never make an exe stale
    : > "$tmp/Source/notes.md"
    [ "$(_freshness "$exe" "$tmp/Source")" = fresh ] || { echo "FAIL: a non-source file counted"; fail=1; }
    # missing exe / missing dirs / empty tree -> indeterminate (never hard-fails)
    [ "$(_freshness "$tmp/nope.exe" "$tmp/Source")" = indeterminate ] || { echo "FAIL: expected indeterminate (exe)"; fail=1; }
    [ "$(_freshness "$exe" "$tmp/missing")" = indeterminate ] || { echo "FAIL: expected indeterminate (dir)"; fail=1; }
    mkdir -p "$tmp/empty"
    [ "$(_freshness "$exe" "$tmp/empty")" = indeterminate ] || { echo "FAIL: expected indeterminate (empty)"; fail=1; }
    if [ "$fail" = 0 ]; then echo "is-exe-fresh --selftest: PASS"; exit 0; fi
    echo "is-exe-fresh --selftest: FAIL"; exit 1
fi

cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 0
PRESET="ninja-iter-msvc"
EXE=""
SRC_DIRS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) PRESET="$2"; shift 2 ;;
        --preset=*) PRESET="${1#--preset=}"; shift ;;
        --exe) EXE="$2"; shift 2 ;;
        --exe=*) EXE="${1#--exe=}"; shift ;;
        --src) SRC_DIRS+=("$2"); shift 2 ;;
        --src=*) SRC_DIRS+=("${1#--src=}"); shift ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "is-exe-fresh: unknown arg: $1" >&2; exit 0 ;;
    esac
done
[ -n "$EXE" ] || EXE="build/$PRESET/Smatchet.exe"
[ "${#SRC_DIRS[@]}" -gt 0 ] || SRC_DIRS=(Source)
SRC_LABEL=""
for _s in "${SRC_DIRS[@]%/}"; do
    if [ -f "$_s" ]; then SRC_LABEL="$SRC_LABEL$_s + "; else SRC_LABEL="$SRC_LABEL$_s/ + "; fi
done
SRC_LABEL="${SRC_LABEL% + }"

RESULT="$(_freshness "$EXE" "${SRC_DIRS[@]}")"
case "$RESULT" in
    stale\ *)
        echo "is-exe-fresh: WARN — $EXE is STALE (a $SRC_LABEL source is newer than the exe)." >&2
        echo "  Newest offending source: ${RESULT#stale }" >&2
        echo "  Rebuild before running: bash scripts/dev/with-msvc-env.sh cmake --build --preset $PRESET --target SmatchetStandalone" >&2
        exit 3 ;;
    indeterminate)
        echo "is-exe-fresh: indeterminate ($EXE or $SRC_LABEL not found) — skipping." >&2
        exit 0 ;;
    *)
        echo "is-exe-fresh: $EXE is fresh (newer than every $SRC_LABEL file)." >&2
        exit 0 ;;
esac
