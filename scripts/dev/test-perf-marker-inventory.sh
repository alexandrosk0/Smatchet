#!/usr/bin/env bash
# test-perf-marker-inventory.sh — Slice 4 of
# docs/plans/shipped/pillar-1-2-perf-review-system.md.
#
# Behaviour (two-tier):
#   1. ADVISORY drift check — if docs/perf/MARKER_INVENTORY.md is stale
#      vs the regenerated content, emit a WARN line pointing at
#      `bash scripts/dev/perf-marker-inventory.sh`. Does NOT fail.
#   2. GATING perf_temp:* leak check — if the REGENERATED inventory lists
#      any in-flight `perf_temp:*` markers, FAIL with exit 1. Temp markers
#      must be stripped before merging (perf-instrument / perf-detective /
#      spike-hunter sessions clean their own markers at end-of-session).
#
# #329: the leak check scans FRESHLY-regenerated content, not the committed
# docs/perf/MARKER_INVENTORY.md — a marker added to the code without
# regenerating the doc would be invisible to a grep of the stale committed file.
#
# selftest: asserts-failure

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --selftest: prove the leak gate scans REGENERATED content, not the committed
# doc. Build a throwaway tree whose committed inventory is CLEAN but whose source
# has a leaked perf_temp:* marker — the gate MUST fail. (The old grep-the-committed-
# file behaviour would PASS on the clean committed doc — this is the #329 witness.)
run_selftest() {
    local tmp
    tmp="$(mktemp -d)"
    # shellcheck disable=SC2064
    trap "rm -rf '$tmp'" RETURN
    mkdir -p "$tmp/tests" "$tmp/docs/perf"
    printf 'void f(){ SMATCHET_UI_PERF_SCOPE("perf_temp:selftest_leak"); }\n' > "$tmp/tests/leak.cpp"
    # Committed inventory is stale/clean (lists no temp markers).
    printf '# perf-marker inventory\n\n## In-flight `perf_temp:*` markers\n\n(none - clean tree)\n' \
        > "$tmp/docs/perf/MARKER_INVENTORY.md"
    if SMATCHET_MARKER_REPO_ROOT="$tmp" bash "$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}")" >/dev/null 2>&1; then
        echo "test-perf-marker-inventory --selftest: FAIL — a perf_temp leak in regenerated content was NOT caught (scanned the stale committed file?)" >&2
        return 1
    fi
    echo "test-perf-marker-inventory --selftest: PASS — a perf_temp leak fails the gate from regenerated content even when the committed inventory is clean."
    return 0
}

if [ "${1:-}" = "--selftest" ]; then
    run_selftest
    exit $?
fi

REPO_ROOT="${SMATCHET_MARKER_REPO_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
cd "$REPO_ROOT" || { echo "ERROR: cd to $REPO_ROOT failed" >&2; exit 1; }

# Tier 1 — advisory drift check (WARNs to stderr, never fails).
bash "$SCRIPT_DIR/perf-marker-inventory.sh" --check 2>&1 || true

# Tier 2 — regenerate the inventory to a throwaway file and scan THAT for leaks
# (#329: never grep the committed doc, which lags the code).
FRESH="$(mktemp -t marker-inventory-fresh-XXXXXX.md)"
# shellcheck disable=SC2064
trap "rm -f '$FRESH'" EXIT
if ! MARKER_INVENTORY_OUT="$FRESH" SMATCHET_MARKER_REPO_ROOT="$REPO_ROOT" \
    bash "$SCRIPT_DIR/perf-marker-inventory.sh" >/dev/null 2>&1; then
    # Under `set -uo pipefail` (no -e) a regen failure would leave $FRESH empty, the
    # leak grep would find nothing, and the gate would exit 0 green — the exact
    # "vanished suite passes" hole this file closes. Fail loudly instead (CR #1734).
    echo "FAIL: perf-marker-inventory.sh regeneration failed — cannot verify the perf_temp leak gate." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

# A leaked marker lands as `| `<path>:<line>` | `perf_temp:...` |` — match that shape.
if grep -qE '^\| `[^`]+` \| `perf_temp:' "$FRESH" 2>/dev/null; then
    echo "FAIL: the regenerated perf-marker inventory lists in-flight perf_temp:* markers — strip before merging." >&2
    grep -E '^\| `[^`]+` \| `perf_temp:' "$FRESH" >&2 || true
    echo "Passed: 0  Failed: 1"
    exit 1
fi

echo "Passed: 1  Failed: 0"
exit 0
