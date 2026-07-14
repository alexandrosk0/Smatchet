#!/usr/bin/env bash
# check-test-list.sh — guard against a tests/Core/*.test.cpp that exists on disk
# but is referenced NOWHERE in tests/CMakeLists.txt: it would be silently
# uncompiled, so its assertions never run yet CI stays green (false green).
# build-quality-velocity-hardening #5.
#
# This is the testable + fast-local companion to the configure-time assert at the
# end of tests/CMakeLists.txt (same glob-vs-text-reference logic). Text-match (not
# the target SOURCES property) so conditionally-gated tests (the Whisper /
# agent-debug blocks) still count as referenced.
#
# Modes:
#   --check     (default) fail (exit 1) listing any unreferenced Core/*.test.cpp.
#   --selftest  fixtures; assert fail-on-unreferenced + pass-on-referenced.
#
# Exit: 0 pass · 1 unreferenced test(s) found / selftest fail · 2 usage.

set -euo pipefail

# unreferenced <core-dir> <cmakelists> -> prints basenames present on disk but
# not mentioned anywhere in the CMakeLists text.
unreferenced() {
    local coredir="$1" cml="$2" f base text
    [ -d "$coredir" ] && [ -f "$cml" ] || return 0
    text="$(cat "$cml")"
    for f in "$coredir"/*.test.cpp; do
        [ -e "$f" ] || continue
        base="$(basename "$f")"
        case "$text" in *"$base"*) : ;; *) printf '%s\n' "$base" ;; esac
    done
}

case "${1:-}" in
    --selftest) MODE=selftest ;;
    --check|"") MODE=check ;;
    *) echo "usage: check-test-list.sh [--check|--selftest]" >&2; exit 2 ;;
esac

if [ "$MODE" = "selftest" ]; then
    fail=0; tmp="$(mktemp -d)" || { echo "check-test-list selftest: mktemp failed" >&2; exit 1; }
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/Core"
    : > "$tmp/Core/Listed.test.cpp"
    : > "$tmp/Core/Unlisted.test.cpp"
    printf 'add_executable(SmatchetTests\n  test_main.cpp\n  Core/Listed.test.cpp\n)\n' > "$tmp/CMakeLists.txt"
    out="$(unreferenced "$tmp/Core" "$tmp/CMakeLists.txt")"
    # selftest: asserts-failure — a known-unlisted test must be flagged (gate's detection path).
    printf '%s\n' "$out" | grep -q '^Unlisted.test.cpp$' || { echo "FAIL: Unlisted.test.cpp not flagged"; fail=1; }
    if printf '%s\n' "$out" | grep -q '^Listed.test.cpp$'; then echo "FAIL: Listed.test.cpp wrongly flagged"; fail=1; fi
    [ "$fail" = 0 ] && { echo "check-test-list --selftest: PASS"; exit 0; } || { echo "check-test-list --selftest: FAIL"; exit 1; }
fi

# --check against the real tree (resolve repo root from this script's location).
cd "$(dirname "${BASH_SOURCE[0]}")/../../.." || exit 2
missing="$(unreferenced tests/Core tests/CMakeLists.txt)"
if [ -n "$missing" ]; then
    echo "check-test-list: FAIL — these tests/Core/*.test.cpp are referenced NOWHERE in tests/CMakeLists.txt" >&2
    echo "  (silently uncompiled = false green):" >&2
    printf '%s\n' "$missing" | sed 's/^/    /' >&2
    echo "  Add each to add_executable(SmatchetTests ...) or the appropriate conditional block." >&2
    exit 1
fi
echo "check-test-list: PASS — every tests/Core/*.test.cpp is referenced in tests/CMakeLists.txt."
exit 0
