#!/usr/bin/env bash
# test-whisper-ai-assistant-autosend.sh — PR #249 (bf9ce67f) regression gate
# for the Whisper dictation → AI Assistant auto-send-on-punctuation flow.
#
# Drives the `whisper-ai-assistant-autosend` scenario which:
#   1. Forces WhisperAutoSendOnPunctuation = true (restored on teardown).
#   2. Registers a TU-local buf with the router using BOTH the AI-flavoured
#      slot (so IsFocusedTargetAiAssistant() returns true) AND a non-zero
#      ItemId (so the splice arms pendingReloadItemId_, the cell that the
#      bf9ce67f fix added).
#   3. Installs an auto-send observer callback.
#   4. Fires whisper.simulate-press + whisper.simulate-release with mock text
#      ending in '.'.
#   5. Asserts:
#      - splice landed (text_match)
#      - pending reload id surfaced with the registered ItemId
#        (`pending_reload_id_matches` — the bf9ce67f fix's protocol half)
#      - auto-send callback fired (`auto_send_fires >= 1`)
#      - Transcribing flag toggled true then back to false
#        (`observed_transcribing_true && transcribing_cleared` — cell 4
#         router-level signal that the menu-bar amber "Transcribing..." reads)
#
# Self-guard: skips silently when the exe is missing or when SMATCHET_WITH_WHISPER=OFF.
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
    echo "[whisper-ai-assistant-autosend] SKIP: $EXE not present (build not run yet)."
    exit 0
fi

# Same probe shape as test-whisper-roundtrip.sh — whisper.status is registered
# only when the plugin is compiled in.
PROBE_OUTPUT=$(SMATCHET_TEST_PORT="$TEST_PORT" "$EXE" cmd commands.list --spawn 2>&1 || true)
if ! grep -q '"whisper.status"' <<<"$PROBE_OUTPUT"; then
    echo "[whisper-ai-assistant-autosend] SKIP: whisper.status not registered (SMATCHET_WITH_WHISPER=OFF)."
    exit 0
fi

echo "[whisper-ai-assistant-autosend] Running whisper-ai-assistant-autosend scenario via $EXE ..."
RESULT=$(SMATCHET_TEST_PORT="$TEST_PORT" "$EXE" cmd scenario.run --name=whisper-ai-assistant-autosend --yes --spawn 2>&1 || true)

echo "$RESULT" | tail -200

# Extract every assertion field via python — fail fast if the envelope is
# missing or any field is wrong.
PY="${PYTHON:-python}"
if ! command -v "$PY" >/dev/null 2>&1; then
    echo "[whisper-ai-assistant-autosend] FAIL: python not on PATH; cannot parse JSON envelope" >&2
    exit 1
fi

parse_field() {
    local field="$1"
    printf '%s' "$RESULT" | "$PY" -c '
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
    if isinstance(data, dict) and "'"$field"'" in data:
        print(data["'"$field"'"])
        sys.exit(0)
print("MISSING")
' 2>/dev/null || echo "MISSING"
}

PASSED=$(parse_field passed)
TEXT_MATCH=$(parse_field text_match)
PENDING_MATCH=$(parse_field pending_reload_id_matches)
AUTO_SENDS=$(parse_field auto_send_fires)
SAW_TX=$(parse_field observed_transcribing_true)
TX_CLEARED=$(parse_field transcribing_cleared)
SCN_STATE=$(parse_field state)

echo
echo "[whisper-ai-assistant-autosend] ----------------- assertions -----------------"
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

assert_eq "text_match (splice landed)"                        "$TEXT_MATCH"   "True"
assert_eq "pending_reload_id_matches (bf9ce67f fix armed)"    "$PENDING_MATCH" "True"
assert_eq "observed_transcribing_true (cell 4 indicator on)"  "$SAW_TX"       "True"
assert_eq "transcribing_cleared (cell 4 indicator off)"       "$TX_CLEARED"   "True"

# auto_send_fires is an int; compare numerically.
if [ "$AUTO_SENDS" != "MISSING" ] && [ "$AUTO_SENDS" -ge 1 ] 2>/dev/null; then
    echo "  PASS  auto_send_fires >= 1 (cell 3 dispatch fired)  ($AUTO_SENDS)"
    PCOUNT=$((PCOUNT + 1))
else
    echo "  FAIL  auto_send_fires >= 1 (cell 3 dispatch fired)  got=$AUTO_SENDS"
    FCOUNT=$((FCOUNT + 1))
fi

assert_eq "scenario reached Asserted state"                   "$SCN_STATE"    "Asserted"
assert_eq "envelope.passed"                                   "$PASSED"       "True"

echo "[whisper-ai-assistant-autosend] -----------------------------------------------"
echo "Passed: $PCOUNT  Failed: $FCOUNT"
if [ "$FCOUNT" -gt 0 ]; then
    exit 1
fi
exit 0
