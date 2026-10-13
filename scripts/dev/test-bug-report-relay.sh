#!/usr/bin/env bash
# test-bug-report-relay.sh — run the bug-report relay's node:test suite (the rate limiters and
# the "minidumps only into a private repo" gate on POST /report). Without this the suite ran only
# on a manual `npm test`, so a change that made the dump gate fail open could merge green and ship
# on the next manual `wrangler deploy`.
#
# No npm install: the tests import only node: built-ins and ../src/index.js. A host without node
# skips (exit 0) rather than failing, since the relay is a separate deployable, not the app.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RELAY="$ROOT/tools/bug-report-relay"

if ! command -v node >/dev/null 2>&1; then
    echo "test-bug-report-relay — SKIP: node not on PATH"
    exit 0
fi

cd "$RELAY"
shopt -s nullglob
SUITES=(test/*.test.mjs)
if [ "${#SUITES[@]}" -eq 0 ]; then
    echo "test-bug-report-relay — no test/*.test.mjs under $RELAY"
    echo "test-bug-report-relay — Passed: 0  Failed: 1"
    exit 1
fi

if node --test "${SUITES[@]}"; then
    echo "test-bug-report-relay — Passed: 1  Failed: 0"
else
    echo "test-bug-report-relay — Passed: 0  Failed: 1"
    exit 1
fi
