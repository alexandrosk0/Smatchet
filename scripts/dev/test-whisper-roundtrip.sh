#!/usr/bin/env bash
# test-whisper-roundtrip.sh — Phase G regression gate for the whisper-dictation
# pipeline. Auto-enrolled by scripts/dev/test-all.sh.
#
# Drives the `whisper-dictation-roundtrip` scenario through a spawned
# Smatchet.exe. The scenario installs a mock-transcription seam in
# WhisperPlugin (see Source/Plugins/Whisper/WhisperPlugin.h § SetMockTranscription)
# so press → release → text-insertion is exercised end-to-end without mic
# hardware or an OpenAI key. Asserts the scenario's JSON report carries
# passed=true.
#
# Self-guard: skips silently when the exe is missing or when the binary was
# built with SMATCHET_WITH_WHISPER=OFF (the scenario is then not registered).
#
# Env overrides:
#   SMATCHET_EXE         path to Smatchet.exe (default build/ninja-iter-msvc/...)
#   SMATCHET_TEST_PORT   MCP port for --spawn (default random in [40000, 60000))
#
# Exit codes:
#   0 — scenario passed=true (or SKIP — no exe / feature off)
#   1 — scenario passed=false / dispatch failure / parse failure

set -euo pipefail

cd "$(dirname "$0")/../.."

EXE="${SMATCHET_EXE:-build/ninja-iter-msvc/Smatchet.exe}"
TEST_PORT="${SMATCHET_TEST_PORT:-$((40000 + RANDOM % 20000))}"

if [ ! -x "$EXE" ] && [ ! -f "$EXE" ]; then
    echo "[whisper-roundtrip] SKIP: $EXE not present (build not run yet)."
    exit 0
fi

# Probe the build for SMATCHET_WITH_WHISPER. `whisper.status` is registered
# only when the plugin is compiled in; on the OFF build the command list
# excludes it and Dispatch returns NotFound. We use the cheap commands.list
# query so this works without the scenario.
PROBE_OUTPUT=$(SMATCHET_TEST_PORT="$TEST_PORT" "$EXE" cmd commands.list --spawn 2>/dev/null || true)
if ! grep -q '"whisper.status"' <<<"$PROBE_OUTPUT"; then
    echo "[whisper-roundtrip] SKIP: whisper.status not registered in this exe (SMATCHET_WITH_WHISPER=OFF)."
    exit 0
fi

echo "[whisper-roundtrip] Running whisper-dictation-roundtrip scenario via $EXE ..."
# scenario.run is marked Destructive in the registry (it can mutate persistent
# UI state for some scenarios), so the CLI envelope demands an explicit --yes.
# Keep stderr OUT of RESULT: the JSON envelope is a single ~8KB stdout line
# (beyond the pipe's atomic-write size), so with 2>&1 the spawn helper's
# "[spawn] ..." stderr lines interleave MID-LINE into the JSON and the
# per-line json.loads parse below fails. Capture stderr to a side file and
# replay it for the log after the envelope.
SPAWN_ERR=$(mktemp)
trap 'rm -f "$SPAWN_ERR"' EXIT
RESULT=$(SMATCHET_TEST_PORT="$TEST_PORT" "$EXE" cmd scenario.run --name=whisper-dictation-roundtrip --yes --spawn 2>"$SPAWN_ERR" || true)

# Pretty-print for the test log. The result envelope is the last JSON object
# in stdout; we accept either pretty or one-line shape.
echo "$RESULT" | tail -200
if [ -s "$SPAWN_ERR" ]; then
    echo "[whisper-roundtrip] --- spawn stderr ---"
    tail -50 "$SPAWN_ERR"
fi

# Extract `data.passed` using a portable POSIX-ish path: prefer python's json
# parser when available, fall back to a grep that matches the literal
# `"passed": true` line emitted by nlohmann::json::dump(2).
PASSED="false"
# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/agents/scripts/core/lib/resolve-py.sh"
PY="$(resolve_py || true)"
if [ -n "$PY" ]; then
    # Pipe the captured envelope through python's JSON parser. The spawn
    # wrapper prints `[spawn] ...` log lines before the JSON envelope; we
    # walk lines bottom-up and parse the first one that loads cleanly as a
    # JSON object carrying `data` or `ok`. Falls through to "false" on any
    # parse error so a malformed envelope can't accidentally pass the gate.
    PASSED=$(printf '%s' "$RESULT" | "$PY" -c '
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
' 2>/dev/null || echo "false")
else
    # Fallback: grep for "passed": true (handles both nlohmann::json::dump(2)
    # multi-line shape and the spawn-wrapper's single-line envelope). Less
    # robust but unblocks environments without a python interpreter.
    if grep -qE '"passed"\s*:\s*true' <<<"$RESULT"; then
        PASSED="true"
    fi
fi

if [ "$PASSED" != "true" ]; then
    echo "[whisper-roundtrip] FAIL: scenario reported passed=$PASSED"
    exit 1
fi

echo "[whisper-roundtrip] PASS"
exit 0
