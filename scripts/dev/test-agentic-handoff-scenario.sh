#!/usr/bin/env bash
# test-agentic-handoff-scenario.sh — H10 end-to-end regression smoke for
# the agentic handoff scenario (`agent-handoff-roundtrip`). Drives the
# scenario via `Smatchet.exe cmd scenario.run --name=agent-handoff-roundtrip
# --spawn` and asserts `passed=true` in the JSON envelope.
#
# The scenario uses :memory: SQLite + a scripted FakeRunner (no real
# subprocess, no GitHub PAT, no HTTP) so CI burns zero credits and the
# smoke is hermetic.
#
# Auto-enrolled by test-all.sh via the test-*.sh glob.
#
# Exit codes:
#   0 — scenario PASSED (passed=true) OR AGENTIC=OFF clean skip
#   1 — scenario failed / envelope malformed
#   2 — exe not found at any known preset location

set -euo pipefail

cd "$(dirname "$0")/../.."

EXE="${SMATCHET_EXE:-./build/ninja-iter-msys2/Smatchet.exe}"
if [ ! -x "$EXE" ]; then
    for alt in \
        ./build/ninja-publish-msys2/Smatchet.exe \
        ./build/ninja-debug-msys2/Smatchet.exe \
        ./build/ninja-test-msys2/Smatchet.exe ; do
        if [ -x "$alt" ]; then
            EXE="$alt"
            break
        fi
    done
fi
if [ ! -x "$EXE" ]; then
    echo "test-agentic-handoff-scenario: Smatchet.exe not built at any common preset"
    exit 2
fi

# Probe registration. If the scenario is not registered the build is
# AGENTIC=OFF; clean skip rather than fail.
SCENARIO_LIST="$("$EXE" cmd scenario.list --spawn 2>&1)" || true
if ! echo "$SCENARIO_LIST" | grep -q "agent-handoff-roundtrip"; then
    echo "[agentic-handoff-scenario] SKIP — agent-handoff-roundtrip not registered (AGENTIC=OFF)"
    exit 0
fi

PASSED=0
FAILED=0

OUT="$("$EXE" cmd scenario.run --name=agent-handoff-roundtrip --yes --spawn 2>&1)" || true

# The scenario.run envelope wraps the IScenario::OnFinish JSON in the
# standard CLI shape: `{"command":"scenario.run","ok":true,"data":{...}}`.
# We grep for the inner `"passed":true` literal — coherent JSON parsing
# is overkill for a smoke + keeps the script portable across MSYS2 sh /
# WSL bash without jq.
if echo "$OUT" | grep -q '"scenario".*"agent-handoff-roundtrip"' \
   && echo "$OUT" | grep -q '"passed":true'; then
    echo "  ok: agent-handoff-roundtrip scenario passed"
    PASSED=$((PASSED + 1))
else
    echo "  FAIL: agent-handoff-roundtrip scenario did not report passed=true"
    echo "  output:"
    echo "$OUT" | sed 's/^/    /'
    FAILED=$((FAILED + 1))
fi

# Sanity check the FSM transitions surfaced in the envelope. The JSON
# object key order is alphabetical (`from`, `ok`, `to`) per nlohmann/json's
# default emitter, so we match the two endpoints loosely rather than
# pinning a fixed key order.
if echo "$OUT" | grep -q '"from":"Pending"' \
   && echo "$OUT" | grep -q '"to":"Spawning"' \
   && echo "$OUT" | grep -q '"from":"PrOpen"' \
   && echo "$OUT" | grep -q '"to":"Complete"'; then
    echo "  ok: expected FSM transitions present in audit trail"
    PASSED=$((PASSED + 1))
else
    echo "  FAIL: expected FSM transitions missing from envelope"
    FAILED=$((FAILED + 1))
fi

echo
echo "Passed: $PASSED  Failed: $FAILED"
if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
