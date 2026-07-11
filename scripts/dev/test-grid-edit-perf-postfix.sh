#!/usr/bin/env bash
# test-grid-edit-perf-postfix.sh — regression gate for grid cell-edit perf.
#
# Drives N synthetic cell commits through ProcessGridFieldEdits and asserts the
# UI-thread cost stays inside pillar-1 (144 Hz, 6.94 ms mean) and pillar-2
# (no UI hitch > 10.0 ms / 100 Hz floor) budgets. See:
#   docs/plans/shipped/grid-cell-edit-perf.md
#   AGENTS.md § UX Pillars
#
# The assertion is that the HTTP commit no longer blocks the UI thread — the
# worker spawn + dispatcher post-back should be sub-millisecond per cell.
#
# Exit codes:
#   0 — mean ≤ 6.94 ms AND p99 ≤ 10.0 ms
#   1 — at least one threshold exceeded
#   2 — exe missing OR Smatchet did not return a parseable JSON envelope

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-iter-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
COUNT="${GRID_EDIT_BURST_COUNT:-200}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-iter-msvc --target SmatchetStandalone" >&2
    echo "Passed: 0  Failed: 0"
    exit 2
fi

echo "[test-grid-edit-perf-postfix] launching ephemeral Smatchet, driving $COUNT synthetic edits..."
RAW_OUTPUT="$("$EXE" cmd debug.grid.edit-burst --count="$COUNT" --spawn --yes 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -25

JSON_LINE="$(echo "$RAW_OUTPUT" | grep -oE '\{.*\}' | tail -1 || true)"
if [ -z "$JSON_LINE" ]; then
    echo "FAIL: could not extract JSON envelope from debug.grid.edit-burst output" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

MEAN="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('per_event_mean_ms','-1'))" 2>/dev/null || echo "-1")"
P99="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('per_event_p99_ms','-1'))" 2>/dev/null || echo "-1")"
MAXMS="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('per_event_max_ms','-1'))" 2>/dev/null || echo "-1")"
ITERS="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('iterations','-1'))" 2>/dev/null || echo "-1")"

echo
echo "Result: iterations=$ITERS mean=${MEAN}ms p99=${P99}ms max=${MAXMS}ms"
echo "Targets: mean ≤ 6.94 ms (Pillar 1)  p99 ≤ 10.0 ms (100 Hz floor)"

if [ "$MEAN" = "-1" ] || [ "$P99" = "-1" ]; then
    echo "Passed: 0  Failed: 1"
    exit 1
fi

OK_MEAN="$("$PY" -c "import sys; print('1' if float('$MEAN') <= 6.94 else '0')")"
OK_P99="$("$PY" -c "import sys; print('1' if float('$P99') <= 10.0 else '0')")"

FAILED=0
PASSED=0
if [ "$OK_MEAN" = "1" ]; then
    PASSED=$((PASSED + 1))
    echo "PASS: mean=${MEAN}ms ≤ 6.94 ms"
else
    FAILED=$((FAILED + 1))
    echo "FAIL: mean=${MEAN}ms > 6.94 ms (Pillar 1 144 Hz budget)"
fi
if [ "$OK_P99" = "1" ]; then
    PASSED=$((PASSED + 1))
    echo "PASS: p99=${P99}ms ≤ 10.0 ms"
else
    FAILED=$((FAILED + 1))
    echo "FAIL: p99=${P99}ms > 10.0 ms (100 Hz floor)"
fi

echo "Passed: $PASSED  Failed: $FAILED"
# Zero-run floor (fail-open shape Z): a run that produces ZERO results leaves
# PASSED=FAILED=0 and would exit green - a vanished/unparsed suite passing.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "$(basename "$0" .sh): FAIL - the test run produced ZERO results (vanished / unparsed)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi
if [ "$FAILED" -ne 0 ]; then
    exit 1
fi
exit 0
