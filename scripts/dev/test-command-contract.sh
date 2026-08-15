#!/usr/bin/env bash
# test-command-contract.sh — registry-wide command error-envelope contract gate.
# Auto-enrolled by scripts/dev/test-all.sh.
#
# Drives the `command-contract-sweep` scenario (gap map Tier 3) through a spawned
# Smatchet.exe. The scenario dispatches every registered command through the real
# CommandRegistry guard path with invalid / unconfirmed inputs and asserts the
# error-envelope contract (missing-required-arg, validation-error, confirm-required,
# unknown-command). Every probe is rejected before any handler runs, so the sweep
# is mutation-free; this script asserts the scenario's JSON report carries ok=true.
#
# Self-guard: skips silently when the exe is missing (build not run yet).
#
# Env overrides:
#   SMATCHET_EXE         path to Smatchet.exe (default build/ninja-iter-msvc/...)
#   SMATCHET_TEST_PORT   MCP port for --spawn (default random in [40000, 60000))
#
# Exit codes:
#   0 — scenario ok=true (or SKIP — no exe)
#   1 — scenario ok=false / dispatch failure / parse failure

set -euo pipefail

cd "$(dirname "$0")/../.."

EXE="${SMATCHET_EXE:-build/ninja-iter-msvc/Smatchet.exe}"
TEST_PORT="${SMATCHET_TEST_PORT:-$((40000 + RANDOM % 20000))}"

if [ ! -x "$EXE" ] && [ ! -f "$EXE" ]; then
    echo "[command-contract] SKIP: $EXE not present (build not run yet)."
    exit 0
fi

echo "[command-contract] Running command-contract-sweep scenario via $EXE ..."
# scenario.run is Destructive in the registry, so the CLI envelope demands --yes.
# stderr stays out of RESULT so spawn log lines cannot interleave into the JSON
# envelope mid-line (same pitfall test-whisper-roundtrip.sh documents).
SPAWN_ERR=$(mktemp)
trap 'rm -f "$SPAWN_ERR"' EXIT
RESULT=$(SMATCHET_TEST_PORT="$TEST_PORT" "$EXE" cmd scenario.run --name=command-contract-sweep --yes --spawn 2>"$SPAWN_ERR" || true)

echo "$RESULT" | tail -100
if [ -s "$SPAWN_ERR" ]; then
    echo "[command-contract] --- spawn stderr ---"
    tail -30 "$SPAWN_ERR"
fi

OK="false"
# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/agents/scripts/core/lib/resolve-py.sh"
PY="$(resolve_py || true)"
if [ -n "$PY" ]; then
    OK=$(printf '%s' "$RESULT" | "$PY" -c '
import json, sys
ok = "false"
buf = sys.stdin.read()
# The envelope is the last parseable JSON object; spawn wrappers may prefix log lines.
for candidate in (buf, buf[buf.find("{"):] if "{" in buf else ""):
    try:
        env = json.loads(candidate)
    except Exception:
        continue
    data = env.get("data", {}) if isinstance(env, dict) else {}
    if isinstance(data, dict) and data.get("ok") is True:
        ok = "true"
    break
print(ok)
')
else
    # Fallback: match the pretty-printed report line.
    if grep -q '"ok": true' <<<"$RESULT"; then
        OK="true"
    fi
fi

if [ "$OK" = "true" ]; then
    echo "[command-contract] PASS — every registered command honoured the error-envelope contract."
    echo "Passed: 1  Failed: 0"
    exit 0
fi

echo "[command-contract] FAIL — contract violations reported (see the violations array above)."
echo "Passed: 0  Failed: 1"
exit 1
