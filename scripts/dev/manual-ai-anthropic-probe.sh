#!/usr/bin/env bash
# manual-ai-anthropic-probe.sh — quota-burning manual diagnostic. Runs
# Smatchet.exe cmd ai.probe --provider anthropic against the user's persisted
# Anthropic API key (NEVER passed on the command line; read from ConfigManager).
#
# NOT auto-enrolled by test-all.sh: the test-all.sh glob is test-*.sh, this
# script's name starts with `manual-` so it opts out by convention.
#
# Exit codes:
#   0 — probe reported ok=true.
#   1 — probe reported ok=false.
#   2 — exe missing.

set -euo pipefail

cd "$(dirname "$0")/../.."

EXE="${SMATCHET_EXE:-./build/ninja-iter-msys2/Smatchet.exe}"
if [ ! -x "$EXE" ]; then
    echo "manual-ai-anthropic-probe: $EXE not built (override with SMATCHET_EXE=...)"
    exit 2
fi

echo "Running ai.probe against Anthropic (via --spawn)..."
OUT="$("$EXE" cmd ai.probe --spawn --provider=anthropic 2>&1)" || true

# Pretty-print and decide.
echo "$OUT"

if echo "$OUT" | grep -q '"ok": true'; then
    echo
    echo "ai.probe: ok=true"
    exit 0
fi

echo
echo "ai.probe: FAILED"
exit 1
