#!/usr/bin/env bash
# manual-grid-edit-perf-compare.sh — run the burst N times and dump human-readable rows.
#
# The `manual-` prefix excludes this from test-all.sh enrolment. Use it when
# tuning the cell-edit pipeline or comparing two builds.
#
# Usage:
#   bash scripts/dev/manual-grid-edit-perf-compare.sh [iterations [runs]]
# Defaults: iterations=200, runs=5.

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-iter-msys2/Smatchet.exe}"
PY="${PYTHON:-python}"
COUNT="${1:-200}"
RUNS="${2:-5}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found." >&2
    exit 2
fi

printf "%-6s %-12s %-12s %-12s %-12s\n" "run" "mean_ms" "p50_ms" "p95_ms" "p99_ms"
printf "%-6s %-12s %-12s %-12s %-12s\n" "---" "-------" "------" "------" "------"

for i in $(seq 1 "$RUNS"); do
    RAW="$("$EXE" cmd debug.grid.edit-burst --count="$COUNT" --spawn --yes 2>&1 || true)"
    JSON_LINE="$(echo "$RAW" | grep -oE '\{.*\}' | tail -1 || true)"
    if [ -z "$JSON_LINE" ]; then
        printf "%-6s %s\n" "$i" "PARSE_FAILED"
        continue
    fi
    MEAN="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('per_event_mean_ms','-'))")"
    P50="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('per_event_p50_ms','-'))")"
    P95="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('per_event_p95_ms','-'))")"
    P99="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('per_event_p99_ms','-'))")"
    printf "%-6s %-12s %-12s %-12s %-12s\n" "$i" "$MEAN" "$P50" "$P95" "$P99"
done
