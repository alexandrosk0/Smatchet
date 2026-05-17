#!/usr/bin/env bash
# manual-ai-anthropic-send.sh — quota-burning manual diagnostic. Drives a
# single Anthropic streaming turn end-to-end via Smatchet.exe cmd ai.send-once.
# Exposes the 400 body verbatim when the configured model / key is rejected,
# which is exactly what surfaced the diagnostic gap that motivated this
# slice.
#
# NOT auto-enrolled by test-all.sh (manual-*.sh prefix opts out).
#
# Exit codes:
#   0 — send reported ok=true.
#   1 — send reported ok=false (look at error_message + response_body_excerpt).
#   2 — exe missing.

set -euo pipefail

cd "$(dirname "$0")/../.."

EXE="${SMATCHET_EXE:-./build/ninja-iter-msys2/Smatchet.exe}"
if [ ! -x "$EXE" ]; then
    echo "manual-ai-anthropic-send: $EXE not built (override with SMATCHET_EXE=...)"
    exit 2
fi

PROMPT="${1:-Hello from Smatchet.}"
echo "Running ai.send-once against Anthropic with prompt: $PROMPT"
OUT="$("$EXE" cmd ai.send-once --spawn --provider=anthropic --prompt="$PROMPT" 2>&1)" || true

echo "$OUT"

if echo "$OUT" | grep -q '"ok": true'; then
    echo
    echo "ai.send-once: ok=true"
    exit 0
fi

echo
echo "ai.send-once: FAILED — check error_message + response_body_excerpt"
exit 1
