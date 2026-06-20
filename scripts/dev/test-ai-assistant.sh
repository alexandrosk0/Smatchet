#!/usr/bin/env bash
# test-ai-assistant.sh — PR-12 headless streaming regression driver for the
# `ai-assistant-send-s2-s4-s5` scenario.
#
# Drives the scenario (Source/Core/src/Commands/Scenarios/AiAssistantSendScenario.cpp)
# which stands up an in-process httplib loopback server and runs the REAL
# OpenAiClient over a real cpr HTTP round-trip across three sub-scenarios:
#   S2 — happy-path streaming   (all deltas + IsFinal, no error)
#   S4 — 401 bad-key error path (onError, HTTP 401, the echoed key redacted)
#   S5 — transport-down         (partial deltas, then non-cancel terminal)
#
# Asserts the per-sub `ok` flags + the top-level `passed` from the JSON envelope.
#
# Self-guard: skips silently when the exe is missing or when SMATCHET_WITH_AI=OFF
# (the scenario is not registered in that build).
#
# Env overrides:
#   SMATCHET_EXE        path to Smatchet.exe (default build/ninja-iter-msvc/...)
#   SMATCHET_TEST_PORT  MCP port for --spawn (default random in [40000, 60000))
#   PYTHON              python interpreter (default `python`)
#
# Exit codes:
#   0 — scenario passed=true (or SKIP — no exe / feature off)
#   1 — scenario passed=false / dispatch failure / parse failure

set -euo pipefail

cd "$(dirname "$0")/../.."

EXE="${SMATCHET_EXE:-build/ninja-iter-msvc/Smatchet.exe}"
TEST_PORT="${SMATCHET_TEST_PORT:-$((40000 + RANDOM % 20000))}"
PY="${PYTHON:-python}"

if [ ! -x "$EXE" ] && [ ! -f "$EXE" ]; then
    echo "[ai-assistant] SKIP: $EXE not present (build not run yet)."
    exit 0
fi

if ! command -v "$PY" >/dev/null 2>&1; then
    echo "[ai-assistant] FAIL: python not on PATH; cannot parse JSON envelope" >&2
    exit 1
fi

# Probe: the scenario is registered only when SMATCHET_WITH_AI is compiled in.
PROBE_OUTPUT=$(SMATCHET_TEST_PORT="$TEST_PORT" "$EXE" cmd scenario.list --spawn 2>/dev/null || true)
if ! grep -q 'ai-assistant-send-s2-s4-s5' <<<"$PROBE_OUTPUT"; then
    echo "[ai-assistant] SKIP: ai-assistant-send-s2-s4-s5 not registered (SMATCHET_WITH_AI=OFF)."
    exit 0
fi

echo "[ai-assistant] Running ai-assistant-send-s2-s4-s5 scenario via $EXE ..."
SPAWN_ERR=$(mktemp)
trap 'rm -f "$SPAWN_ERR"' EXIT
RESULT=$(SMATCHET_TEST_PORT="$TEST_PORT" "$EXE" cmd scenario.run \
    --name=ai-assistant-send-s2-s4-s5 --yes --spawn 2>"$SPAWN_ERR" || true)

echo "$RESULT" | tail -200
if [ -s "$SPAWN_ERR" ]; then
    echo "[ai-assistant] --- spawn stderr ---"
    tail -50 "$SPAWN_ERR"
fi

# Extract a nested field (dotted path under data) via python — fail closed on a
# missing envelope or absent field.
parse_field() {
    local path="$1"
    printf '%s' "$RESULT" | "$PY" -c '
import json, sys
path = sys.argv[1].split(".")
lines = [ln for ln in sys.stdin.read().splitlines() if ln.strip().startswith("{")]
for ln in reversed(lines):
    try:
        obj = json.loads(ln)
    except Exception:
        continue
    if not isinstance(obj, dict):
        continue
    cur = obj.get("data", obj)
    ok = True
    for key in path:
        if isinstance(cur, dict) and key in cur:
            cur = cur[key]
        else:
            ok = False
            break
    if ok:
        print(cur)
        sys.exit(0)
print("MISSING")
' "$path" 2>/dev/null || echo "MISSING"
}

S2_OK=$(parse_field s2_happy_path.ok)
S4_OK=$(parse_field s4_bad_key_401.ok)
S4_LEAKED=$(parse_field s4_bad_key_401.keyLeaked)
S5_OK=$(parse_field s5_transport_down.ok)
PASSED=$(parse_field passed)

echo
echo "[ai-assistant] ----------------- assertions -----------------"
PCOUNT=0
FCOUNT=0
assert_eq() {
    local label="$1" got="$2" expect="$3"
    if [ "$got" = "$expect" ]; then
        echo "  PASS  $label  ($got)"
        PCOUNT=$((PCOUNT + 1))
    else
        echo "  FAIL  $label  got=$got expected=$expect"
        FCOUNT=$((FCOUNT + 1))
    fi
}

assert_eq "S2 happy-path streaming ok"            "$S2_OK"     "True"
assert_eq "S4 401 bad-key error-path ok"          "$S4_OK"     "True"
assert_eq "S4 echoed key was NOT leaked"          "$S4_LEAKED" "False"
assert_eq "S5 transport-down ok"                  "$S5_OK"     "True"
assert_eq "envelope.passed"                       "$PASSED"    "True"

echo "[ai-assistant] -----------------------------------------------"
echo "Passed: $PCOUNT  Failed: $FCOUNT"
if [ "$FCOUNT" -gt 0 ]; then
    exit 1
fi
exit 0
