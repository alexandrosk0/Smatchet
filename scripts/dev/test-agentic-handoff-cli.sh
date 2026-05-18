#!/usr/bin/env bash
# H3 — agentic handoff CLI smoke. Skips cleanly when:
#   - The Smatchet exe is missing (e.g. ninja-iter-msys2 hasn't run yet).
#   - SMATCHET_WITH_AGENTIC=OFF — the handoff command (lands in H4) isn't
#     registered in the AGENTIC=OFF build, so the smoke has nothing to test.
#
# H3 ships the runner primitives only; the `handoff.start` / `handoff.dry-run`
# command itself lands in H4 with `AgenticHandoffController`. Until then the
# smoke verifies that the AGENTIC=ON build still parses `cmd commands.list`
# without error — the runner / seed-builder are linked but not yet
# command-registered. Once H4 lands, this script's grep narrows from the
# generic "AGENTIC commands present" check to "handoff command registered".

set -euo pipefail

EXE_CANDIDATES=(
    "build/ninja-iter-msys2/Smatchet.exe"
    "build/ninja-debug-msys2/Smatchet.exe"
    "build/ninja-publish-msys2/Smatchet.exe"
)

EXE=""
for candidate in "${EXE_CANDIDATES[@]}"; do
    if [[ -x "$candidate" ]]; then
        EXE="$candidate"
        break
    fi
done

if [[ -z "$EXE" ]]; then
    echo "[agentic-handoff-cli] SKIP — no Smatchet.exe in build/ninja-*-msys2/"
    exit 0
fi

# Pull the registered command list; the exe interleaves `[spawn] ...` markers
# inline with the JSON response, so we substring-match rather than parse.
# Pattern mirrors the existing test-agentic-triage-cli.sh shape.
RAW=$("$EXE" cmd commands.list --spawn 2>&1 || true)

# H3-only marker: AGENTIC-gated commands appear at all. From H4 onward the
# substring narrows to `handoff.start` / `handoff.dry-run`. Until then the
# `agent.triage.run` entry suffices to confirm AGENTIC=ON command registration.
if ! echo "$RAW" | grep -q '"name":"agent\.triage\.run"' ; then
    echo "[agentic-handoff-cli] SKIP — no agentic commands registered (SMATCHET_WITH_AGENTIC=OFF or pre-H4)"
    exit 0
fi

# Sanity: the response envelope's command field is present + ok=true.
if ! echo "$RAW" | grep -q '"command":"commands\.list"' ; then
    echo "[agentic-handoff-cli] FAIL — commands.list command response not found in output"
    echo "$RAW" | head -3
    exit 1
fi
if ! echo "$RAW" | grep -q '"ok":true' ; then
    echo "[agentic-handoff-cli] FAIL — commands.list envelope did not report ok=true"
    exit 1
fi

echo "[agentic-handoff-cli] PASS — AGENTIC commands present in registry"
exit 0
