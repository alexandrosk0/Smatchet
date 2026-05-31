#!/usr/bin/env bash
# manual-ai-lmstudio-send.sh — manual diagnostic for the LM Studio /
# OpenAI-compatible local-server path. Drives a single OllamaOpenAiCompat
# streaming turn end-to-end via Smatchet.exe cmd ai.send-once.
#
# Expects a local OpenAI-compatible server (LM Studio default endpoint is
# http://localhost:1234/v1) with at least one model loaded. The provider's
# Combo display label was widened to "OpenAI-compatible local (LM Studio /
# Ollama / LocalAI / vLLM)" to surface this path; this script proves the
# wire-level happy path against LM Studio specifically.
#
# Set base URL + model up-front via Preferences (Assistant tab) before
# running, OR override via env: SMATCHET_AI_BASEURL + SMATCHET_AI_MODEL.
#
# NOT auto-enrolled by test-all.sh (manual-*.sh prefix opts out).
#
# Exit codes:
#   0 — send reported ok=true.
#   1 — send reported ok=false (look at error_message + response_body_excerpt).
#   2 — exe missing.

set -euo pipefail

cd "$(dirname "$0")/../.."

EXE="${SMATCHET_EXE:-./build/ninja-iter-msvc/Smatchet.exe}"
if [ ! -x "$EXE" ]; then
    echo "manual-ai-lmstudio-send: $EXE not built (override with SMATCHET_EXE=...)"
    exit 2
fi

PROMPT="${1:-Hello from Smatchet via LM Studio.}"
echo "Running ai.send-once against OllamaOpenAiCompat with prompt: $PROMPT"
echo "(Make sure your local OpenAI-compatible server is running, e.g. LM Studio on http://localhost:1234/v1)"
OUT="$("$EXE" cmd ai.send-once --spawn --provider=ollama-openai --prompt="$PROMPT" 2>&1)" || true

echo "$OUT"

if echo "$OUT" | grep -q '"ok": true'; then
    echo
    echo "ai.send-once: ok=true"
    exit 0
fi

echo
echo "ai.send-once: FAILED — check error_message + response_body_excerpt"
echo "Common LM Studio issues: server not running; no model loaded; wrong Base URL in Preferences"
exit 1
