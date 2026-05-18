#!/usr/bin/env bash
# test-agentic-approve-reject.sh — T8 regression gate for the agent-triage
# roundtrip scenario. Drives the `agent-triage-roundtrip` scenario through a
# spawned Smatchet.exe and asserts the JSON envelope carries passed=true.
#
# The scenario runs entirely against in-process mocks (no live GitHub HTTP,
# no Ollama runtime), so the test consumes zero credits and zero network.
#
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
#
# Self-guard: skips silently when the exe is missing or when the binary was
# built with SMATCHET_WITH_AGENTIC=OFF (the scenario is then not registered).
#
# Env overrides:
#   SMATCHET_EXE        path to Smatchet.exe (default build/ninja-iter-msys2/...)
#   SMATCHET_TEST_PORT  MCP port for --spawn (default random in [40000, 60000))
#
# Exit codes:
#   0 — scenario passed=true (or SKIP — no exe / AGENTIC=OFF)
#   1 — scenario passed=false / dispatch failure / parse failure

set -euo pipefail

cd "$(dirname "$0")/../.."

EXE="${SMATCHET_EXE:-build/ninja-iter-msys2/Smatchet.exe}"
TEST_PORT="${SMATCHET_TEST_PORT:-$((40000 + RANDOM % 20000))}"

if [ ! -x "$EXE" ] && [ ! -f "$EXE" ]; then
    for alt in \
        build/ninja-publish-msys2/Smatchet.exe \
        build/ninja-debug-msys2/Smatchet.exe \
        build/ninja-test-msys2/Smatchet.exe ; do
        if [ -f "$alt" ]; then
            EXE="$alt"
            break
        fi
    done
fi
if [ ! -f "$EXE" ]; then
    echo "[agentic-approve-reject] SKIP: $EXE not present (build not run yet)."
    exit 0
fi

# Probe the build for SMATCHET_WITH_AGENTIC. The scenario factory is only
# registered when the option is ON; the AGENTIC=OFF build still registers a
# stub agent.triage.run CLI handler via BuiltinCommands_Agentic.cpp's #else
# branch, so checking commands.list for it is not a reliable gate. Use
# scenario.list — which returns the registered scenario names — as the
# AGENTIC discriminator.
PROBE_OUTPUT="$(SMATCHET_TEST_PORT="$TEST_PORT" "$EXE" cmd scenario.list --spawn 2>&1 || true)"
if ! grep -q 'agent-triage-roundtrip' <<<"$PROBE_OUTPUT"; then
    echo "[agentic-approve-reject] SKIP: agent-triage-roundtrip scenario not registered (SMATCHET_WITH_AGENTIC=OFF)."
    exit 0
fi

echo "[agentic-approve-reject] Running agent-triage-roundtrip scenario via $EXE ..."
# scenario.run is registered Destructive (it can mutate persistent UI state for
# some scenarios), so the CLI envelope demands an explicit --yes.
RESULT="$(SMATCHET_TEST_PORT="$TEST_PORT" "$EXE" cmd scenario.run \
    --name=agent-triage-roundtrip --frames=12 --yes --spawn 2>&1 || true)"

# Pretty-print for the test log. The result envelope is the last JSON object
# in stdout; the spawn wrapper writes the OnFinish payload as `data: { ... }`.
echo "$RESULT" | tail -200

# Extract `data.passed` using python's JSON parser (preferred) or fall back to
# a literal grep. The spawn wrapper prints `[spawn] ...` log lines before the
# JSON envelope so we filter to lines starting with `{` and try bottom-up.
PASSED="false"
if command -v python >/dev/null 2>&1; then
    PASSED="$(printf '%s' "$RESULT" | python -c '
import json, sys
lines = [ln for ln in sys.stdin.read().splitlines() if ln.strip().startswith("{")]
for ln in reversed(lines):
    try:
        obj = json.loads(ln)
    except Exception:
        continue
    if not isinstance(obj, dict):
        continue
    data = obj.get("data", obj)
    passed = data.get("passed") if isinstance(data, dict) else None
    if isinstance(passed, bool):
        print("true" if passed else "false")
        sys.exit(0)
print("false")
' 2>/dev/null || echo "false")"
else
    if grep -qE '"passed"\s*:\s*true' <<<"$RESULT"; then
        PASSED="true"
    fi
fi

if [ "$PASSED" != "true" ]; then
    echo "[agentic-approve-reject] FAIL: scenario reported passed=$PASSED"
    exit 1
fi

echo "[agentic-approve-reject] PASS"
exit 0
