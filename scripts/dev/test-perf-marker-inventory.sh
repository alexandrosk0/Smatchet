#!/usr/bin/env bash
# test-perf-marker-inventory.sh — Slice 4 of
# docs/design/pillar-1-2-perf-review-system.md.
#
# Behaviour (two-tier):
#   1. ADVISORY drift check — if docs/perf/MARKER_INVENTORY.md is stale
#      vs the regenerated content, emit a WARN line pointing at
#      `bash scripts/dev/perf-marker-inventory.sh`. Does NOT fail.
#   2. GATING perf_temp:* leak check — if the regenerated inventory
#      lists any in-flight `perf_temp:*` markers, FAIL with exit 1.
#      Temp markers must be stripped before merging (perf-instrument
#      / perf-detective / spike-hunter sessions are responsible for
#      cleaning their own markers at end-of-session).

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Hard-fail on cd: the perf-marker-inventory.sh subcall does relative
# `grep -rnE` walks; a failed cd would silently scan the wrong tree and
# the test would falsely pass.
cd "$REPO_ROOT" || { echo "ERROR: cd to $REPO_ROOT failed" >&2; exit 1; }

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
