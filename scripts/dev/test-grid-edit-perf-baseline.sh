#!/usr/bin/env bash
# test-grid-edit-perf-baseline.sh — informational baseline capture for grid cell-edit perf.
#
# Runs the same burst as test-grid-edit-perf-postfix.sh but does NOT assert.
# Writes the JSON envelope to stdout so a human can compare pre-/post-fix
# numbers. Auto-enrolled in test-all.sh (no `manual-` prefix) but exits 0
# unconditionally as long as the JSON parses.
#
# Exit codes:
#   0 — envelope parsed (no thresholds enforced)
#   2 — exe missing OR Smatchet did not return a parseable JSON envelope

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-iter-msys2/Smatchet.exe}"
PY="${PYTHON:-python}"
COUNT="${GRID_EDIT_BURST_COUNT:-200}"

if [ ! -f "$EXE" ]; then
    echo "SKIP: $EXE not found (baseline collection requires build)" >&2
    echo "Passed: 0  Failed: 0"
    exit 2
fi

echo "[test-grid-edit-perf-baseline] capturing baseline ($COUNT iterations)..."
RAW_OUTPUT="$("$EXE" cmd debug.grid.edit-burst --count="$COUNT" --spawn --yes 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -25

JSON_LINE="$(echo "$RAW_OUTPUT" | grep -oE '\{.*\}' | tail -1 || true)"
if [ -z "$JSON_LINE" ]; then
    echo "Passed: 0  Failed: 1"
    exit 2
fi

MEAN="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('per_event_mean_ms','-1'))" 2>/dev/null || echo "-1")"
P99="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('per_event_p99_ms','-1'))" 2>/dev/null || echo "-1")"
echo "Baseline: mean=${MEAN}ms p99=${P99}ms (informational; no assertion)"
echo "Passed: 1  Failed: 0"
exit 0
