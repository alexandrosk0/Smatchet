#!/usr/bin/env bash
# test-whisper-roundtrip.sh — Phase G regression gate for the whisper-dictation
# pipeline. Auto-enrolled by scripts/dev/test-all.sh.
#
# Drives the `whisper-dictation-roundtrip` scenario through a spawned
# Smatchet.exe. The scenario installs a mock-transcription seam in
# WhisperPlugin (see Source/Plugins/Whisper/WhisperPlugin.h § SetMockTranscription)
# so press → release → text-insertion is exercised end-to-end without mic
# hardware or an OpenAI key.
#
# Two variants run, each in its own spawned exe:
#   1. a clip that transcribes to text: the text lands in the focused field;
#   2. an empty clip (--expectNoSpeechNotice=true): nothing is typed and the
#      user gets the "no speech" notice instead of silence.
# Each variant's JSON report must carry passed=true AND show that the variant
# asked for is the one that ran (expect_no_speech_notice, notice_seen): the
# scenario passes in either mode, so passed=true alone cannot tell them apart.
#
# Self-guard: skips silently when the exe is missing or when the binary was
# built with SMATCHET_WITH_WHISPER=OFF (the scenario is then not registered).
#
# Env overrides:
#   SMATCHET_EXE         path to Smatchet.exe (default build/ninja-iter-msvc/...)
#   SMATCHET_TEST_PORT   MCP port for --spawn (default random in [40000, 60000))
#
# Exit codes:
#   0 — both variants passed and ran as asked (or SKIP — no exe / feature off)
#   1 — a variant reported passed=false, ran the other variant, or failed to
#       dispatch / parse

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

SPAWN_ERR=$(mktemp)
trap 'rm -f "$SPAWN_ERR"' EXIT

# resolve-py.sh is agent-layer content:
# the agent-layer/ mount once it holds the layer's project-config.sh (plan
# agent-surface-extraction-repo, row 12), else this tree.
_twr_layer="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ -f "$_twr_layer/agent-layer/scripts/dev/project-config.sh" ] && _twr_layer="$_twr_layer/agent-layer"
# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$_twr_layer/agents/scripts/core/lib/resolve-py.sh"
PY="$(resolve_py || true)"

# run_variant <label> <mode> [extra scenario args...]
# Runs the scenario once in a spawned exe and returns 0 only when its report
# carries passed=true AND shows the variant that was asked for actually ran.
# <mode> is "text" or "no-speech". The scenario passes in either mode, so
# passed=true alone would also be reported if the extra args never reached it
# and it ran the text variant twice: for "no-speech" the report must carry
# expect_no_speech_notice=true and notice_seen=true, for "text" it must not
# carry expect_no_speech_notice=true.
run_variant() {
    local label="$1"
    local mode="$2"
    shift 2
    echo "[whisper-roundtrip] Running whisper-dictation-roundtrip ($label) via $EXE ..."
    # scenario.run is marked Destructive in the registry (it can mutate persistent
    # UI state for some scenarios), so the CLI envelope demands an explicit --yes.
    # Keep stderr OUT of the result: the JSON envelope is a single ~8KB stdout line
    # (beyond the pipe's atomic-write size), so with 2>&1 the spawn helper's
    # "[spawn] ..." stderr lines interleave MID-LINE into the JSON and the
    # per-line json.loads parse below fails. Capture stderr to a side file and
    # replay it for the log after the envelope.
    local result
    result=$(SMATCHET_TEST_PORT="$TEST_PORT" "$EXE" cmd scenario.run --name=whisper-dictation-roundtrip "$@" --yes --spawn 2>"$SPAWN_ERR" || true)

    # Pretty-print for the test log. The result envelope is the last JSON object
    # in stdout; we accept either pretty or one-line shape.
    echo "$result" | tail -200
    if [ -s "$SPAWN_ERR" ]; then
        echo "[whisper-roundtrip] --- spawn stderr ---"
        tail -50 "$SPAWN_ERR"
    fi

    # Extract `data.passed` using a portable POSIX-ish path: prefer python's json
    # parser when available, fall back to a grep that matches the literal
    # `"passed": true` line emitted by nlohmann::json::dump(2). Either path
    # yields "true", "false", or "wrong-mode" (passed, but not the variant asked for).
    local passed="false"
    if [ -n "$PY" ]; then
        # Pipe the captured envelope through python's JSON parser. The spawn
        # wrapper prints `[spawn] ...` log lines before the JSON envelope; we
        # walk lines bottom-up and parse the first one that loads cleanly as a
        # JSON object carrying `data` or `ok`. Falls through to "false" on any
        # parse error so a malformed envelope can't accidentally pass the gate.
        passed=$(printf '%s' "$result" | "$PY" -c '
import json, sys
mode = sys.argv[1]
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
        no_speech = data.get("expect_no_speech_notice") is True
        if mode == "no-speech":
            ran = no_speech and data.get("notice_seen") is True
        else:
            ran = not no_speech
        print(("true" if ran else "wrong-mode") if passed else "false")
        sys.exit(0)
print("false")
' "$mode" 2>/dev/null || echo "false")
    else
        # Fallback: grep for "passed": true (handles both nlohmann::json::dump(2)
        # multi-line shape and the spawn-wrapper's single-line envelope). Less
        # robust but unblocks environments without a python interpreter.
        if grep -qE '"passed"\s*:\s*true' <<<"$result"; then
            passed="true"
            local no_speech="false"
            if grep -qE '"expect_no_speech_notice"\s*:\s*true' <<<"$result"; then
                no_speech="true"
            fi
            if [ "$mode" = "no-speech" ]; then
                if [ "$no_speech" != "true" ] || ! grep -qE '"notice_seen"\s*:\s*true' <<<"$result"; then
                    passed="wrong-mode"
                fi
            elif [ "$no_speech" = "true" ]; then
                passed="wrong-mode"
            fi
        fi
    fi

    if [ "$passed" = "wrong-mode" ]; then
        echo "[whisper-roundtrip] FAIL ($label): the scenario passed, but its report does not show the '$mode' variant ran (expect_no_speech_notice / notice_seen)"
        return 1
    fi
    if [ "$passed" != "true" ]; then
        echo "[whisper-roundtrip] FAIL ($label): scenario reported passed=$passed"
        return 1
    fi
    echo "[whisper-roundtrip] ok ($label)"
    return 0
}

run_variant "text lands in the focused field" text || exit 1
run_variant "empty clip raises the no-speech notice" no-speech --expectNoSpeechNotice=true || exit 1

echo "[whisper-roundtrip] PASS"
exit 0
