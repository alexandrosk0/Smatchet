#!/usr/bin/env bash
# test-config-globs.sh — assert every project.config.json glob that is supposed to
# select tracked source actually matches >=1 tracked file.
#
# WHY THIS EXISTS (#4)
#   project.config.json § visual_validation.trigger_globs drives the Pillar-4
#   visual-validation pause (AGENTS.md § Autonomous ship-loop default: a change
#   touching these paths with no bucket-C/E coverage pauses for human verify).
#   A glob that matches ZERO tracked files is dead config — it silently never
#   triggers, so a localization/theme edit ships without the visual gate. The
#   shipped example: `**/Locales/*.json` matched nothing (the Locales/*.json
#   override files are user-supplied runtime assets next to the exe, never
#   committed — README § localization), so the visual gate never fired on a
#   French-string edit. This guard fails CLOSED on any zero-match glob so a
#   future path drift (a renamed/moved source) is caught at PR time.
#
# MODES
#   (no args) / --check   fail if any checked glob matches zero tracked files.
#   --selftest            self-contained proof: a deliberately-dead glob is
#                         detected as zero-match (gate works), a known-live glob
#                         is detected as matching. Exits 0 iff both hold.
#
# EXIT  0 every checked glob is live · 1 a zero-match (dead) glob · 2 infra error.

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT" || { echo "test-config-globs: cannot cd to repo root" >&2; exit 2; }

CONFIG="project.config.json"
[ -f "$CONFIG" ] || { echo "test-config-globs: missing $CONFIG" >&2; exit 2; }

# command -v alone is insufficient on Windows: the python3 Store-alias stub passes
# it but exits 49 when run. Probe each candidate; take the first that executes.
PY=""
for _c in python3 python py; do
    _p="$(command -v "$_c" 2>/dev/null)" || continue
    if "$_p" -c "" >/dev/null 2>&1; then PY="$_p"; break; fi
done
[ -n "$PY" ] || { echo "test-config-globs: no working python interpreter" >&2; exit 2; }

# Count tracked files matching a git pathspec glob (e.g. **/Foo.cpp). `git ls-files`
# uses fnmatch glob semantics for the pathspec, so **/ matches at any depth.
glob_count() {
    git ls-files -- "$1" 2>/dev/null | grep -c . || true
}

# Emit the visual_validation.trigger_globs entries, one per line. Write raw bytes with
# explicit LF: Python's text-mode print() emits CRLF on Windows, and a trailing \r in a
# git pathspec (`**/Foo.cpp\r`) silently matches nothing — the exact false-pass this guards.
list_visual_globs() {
    "$PY" - "$CONFIG" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1], encoding="utf-8"))
for g in cfg.get("visual_validation", {}).get("trigger_globs", []):
    sys.stdout.buffer.write((g + "\n").encode("utf-8"))  # raw LF — text mode would CRLF on Windows
PY
}

run_check() {
    local rc=0 g n
    while IFS= read -r g; do
        [ -n "$g" ] || continue
        n="$(glob_count "$g")"
        if [ "$n" -eq 0 ]; then
            echo "FAIL: visual_validation glob '$g' matches ZERO tracked files (dead config)." >&2
            echo "      Point it at the real tracked source, or drop it from project.config.json." >&2
            rc=1
        else
            echo "[test-config-globs] OK '$g' -> $n tracked file(s)"
        fi
    done < <(list_visual_globs)
    [ "$rc" -eq 0 ] && echo "PASS — every visual_validation glob matches >=1 tracked file."
    return "$rc"
}

run_selftest() {
    local miss=0
    # selftest: asserts-failure — a deliberately-dead glob must be detected as zero-match (the bug class this guards).
    if [ "$(glob_count '**/Locales/*.json')" -ne 0 ]; then
        echo "SELFTEST FAIL: the historically-dead glob '**/Locales/*.json' now matches tracked files"
        echo "               — if Locales/*.json are now committed, update this selftest fixture." >&2
        miss=1
    fi
    # A known-live glob must be detected as matching (proves glob_count isn't always-zero).
    if [ "$(glob_count '**/SmatchetLocalization.cpp')" -eq 0 ]; then
        echo "SELFTEST FAIL: known-live glob '**/SmatchetLocalization.cpp' matched zero tracked files" >&2
        miss=1
    fi
    [ "$miss" -eq 0 ] && { echo "selftest: zero-match detection works (dead glob caught, live glob matched)"; return 0; }
    return 1
}

case "${1:---check}" in
    --check)    run_check ;;
    --selftest) run_selftest ;;
    *) echo "usage: $0 [--check|--selftest]" >&2; exit 2 ;;
esac
