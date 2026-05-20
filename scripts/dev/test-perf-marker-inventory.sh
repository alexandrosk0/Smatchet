#!/usr/bin/env bash
# test-perf-marker-inventory.sh — advisory check that the checked-in
# docs/perf/MARKER_INVENTORY.md matches the live tree's set of
# SMATCHET_UI_PERF_SCOPE markers. Slice 4 of
# docs/design/pillar-1-2-perf-review-system.md.
#
# This test ALWAYS exits 0 — it's a hygiene reminder, not a gating signal.
# A drift WARN points an agent at `bash scripts/dev/perf-marker-inventory.sh`
# to regenerate the doc.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

OUTPUT="$(bash scripts/dev/perf-marker-inventory.sh --check 2>&1)"
echo "$OUTPUT"

# Detect a perf_temp: LEAK — only count actual marker-table rows, not the
# header / commentary text that legitimately mentions `perf_temp:`. A leaked
# marker would land as `| \`<path>:<line>\` | \`perf_temp:...\` |` — match
# that exact shape.
if grep -qE '^\| `[^`]+` \| `perf_temp:' docs/perf/MARKER_INVENTORY.md 2>/dev/null; then
    echo "FAIL: docs/perf/MARKER_INVENTORY.md lists in-flight perf_temp:* markers — strip before merging." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

echo "Passed: 1  Failed: 0"
exit 0
