#!/usr/bin/env bash
# test-orphan-bats.sh — every tests/bats/*.bats must be run by a test-*.sh
# wrapper, so an added suite can't silently never-run (the gap that nearly shipped
# capture_intent.bats un-run; pr-intent-capture-hardening #3).
#
# scripts/dev/test-all.sh discovers tests by the test-*.sh glob and never runs a
# .bats directly, so a .bats with no wrapper naming it is dead weight CI never
# executes. A .bats is "wrapped" iff some test-*.sh under the test roots references
# its basename (a wrapper does BATS_FILE="tests/bats/<name>.bats"; bats "$BATS_FILE").
#
#   --selftest : create a throwaway unreferenced .bats and assert it is flagged.
#   (no arg)   : check tests/bats/; exit 1 on any orphan.
#
# Exit: 0 clean · 1 orphan(s) found · 2 cannot resolve repo root.
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "test-orphan-bats: cannot resolve repo root" >&2
    exit 2
}
cd "$ROOT" || { echo "test-orphan-bats: cannot cd to repo root" >&2; exit 2; }

# Test roots scripts/dev/test-all.sh discovers wrappers from.
WRAPPER_ROOTS=(scripts/dev agents/scripts/core agents/scripts/project)

# Is bats basename $1 RUN by any test-*.sh wrapper under the roots? A real wrapper
# names the suite by its tests/bats/<name> PATH (BATS_FILE="tests/bats/<name>.bats";
# bats "$BATS_FILE"). Match that path on a NON-comment line so a bare basename or a
# path mention inside a comment — or a different suite whose name is a substring —
# cannot false-pass an unwrapped suite (Bugbot #10044780: grep -F on the bare
# basename matched comment mentions / partial filenames).
_is_wrapped() {
    local base="$1" root
    for root in "${WRAPPER_ROOTS[@]}"; do
        [ -d "$root" ] || continue
        if grep -rhF "tests/bats/$base" "$root" --include='test-*.sh' 2>/dev/null \
             | grep -qvE '^[[:space:]]*#'; then
            return 0
        fi
    done
    return 1
}

# Check a bats dir (default tests/bats); print orphans to stderr; return 1 if any.
_check_dir() {
    local dir="$1" rc=0 f base
    for f in "$dir"/*.bats; do
        [ -e "$f" ] || continue
        base="$(basename "$f")"
        if ! _is_wrapped "$base"; then
            echo "  - $f (no test-*.sh wrapper runs it — add one: bats $f)" >&2
            rc=1
        fi
    done
    return "$rc"
}

if [ "${1:-}" = "--selftest" ]; then
    # selftest: asserts-failure — feeds a known-bad input (an unwrapped .bats whose
    # path appears ONLY in a wrapper COMMENT) and asserts the checker still flags
    # it; a regression to comment/basename matching (Bugbot #10044780) trips this.
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/bats" "$tmp/wrappers"
    # Dynamic sentinel: its RESOLVED name must not appear literally in this file,
    # else _is_wrapped greps THIS test-*.sh and self-reports the sentinel as wrapped.
    sentinel="_orphan_$$_${RANDOM}.bats"
    : > "$tmp/bats/$sentinel"
    # Decoy wrapper that only MENTIONS the suite path in a comment — must NOT count.
    printf '#!/usr/bin/env bash\n# decoy: references tests/bats/%s in a comment only\n' \
        "$sentinel" > "$tmp/wrappers/test-decoy-bats.sh"
    WRAPPER_ROOTS=("$tmp/wrappers")
    if _check_dir "$tmp/bats" 2>/dev/null; then
        echo "test-orphan-bats --selftest: FAIL — a comment-only mention counted as a wrapper" >&2
        echo "Passed: 0  Failed: 1"; exit 1
    fi
    echo "test-orphan-bats --selftest: PASS — unwrapped .bats flagged; comment mention ignored."
    echo "Passed: 1  Failed: 0"; exit 0
fi

if _check_dir "tests/bats"; then
    n="$(find tests/bats -maxdepth 1 -name '*.bats' | wc -l | tr -d ' ')"
    echo "test-orphan-bats: PASS — all $n bats suite(s) have a test-*.sh wrapper."
    echo "Passed: 1  Failed: 0"; exit 0
else
    echo "test-orphan-bats: FAIL — bats suite(s) above have no wrapper (never run in CI)." >&2
    echo "Passed: 0  Failed: 1"; exit 1
fi
