#!/usr/bin/env bash
# H5 — clarification dual-channel CLI smoke. Drives the new clarification
# round-trip surface end-to-end via the Smatchet exe. Skips cleanly when:
#   - The Smatchet exe is missing (e.g. ninja-iter-msys2 hasn't run yet).
#   - SMATCHET_WITH_AGENTIC=OFF — the handoff commands resolve to stub
#     handlers that always fail; the smoke can't distinguish that from a
#     real failure, so it skips.
#
# H5 surface checked:
#   1. `commands.list` contains `handoff.clarify` (registered in the AGENTIC
#      build only).
#   2. `handoff.clarify --proposal_id=99999 --answer=foo` fails because the
#      proposal does not exist — the error path is the H5 contract that the
#      CLI surfaces a friendly message rather than crashing the registry.
#   3. `handoff.dry-run --proposal_id=99999 --use_stub_claude=true` round-trips
#      cleanly (already exercised by test-agentic-handoff-cli.sh; we repeat
#      it as a smoke ping that the AGENTIC build still wires correctly under
#      the H5 extension).
#
# The full clarification path (stub_claude --stub-mode=clarification end-to-end)
# is exercised by the doctest rig in tests/Source_Core/AgenticHandoffController.test.cpp
# under controlled threading; running it from a shell would require a real
# proposal insertion CLI which is out of scope for H5. The smoke here verifies
# the CLI surface is registered + the registry happy-path stays green.

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
    echo "[agentic-handoff-clarification] SKIP — no Smatchet.exe in build/ninja-*-msys2/"
    exit 0
fi

LIST_OUT=$("$EXE" cmd commands.list --spawn 2>&1 || true)

# AGENTIC=OFF gates every handoff.* command to a stub that always fails.
if ! echo "$LIST_OUT" | grep -q '"name":"agent\.triage\.run"' ; then
    echo "[agentic-handoff-clarification] SKIP — no agentic commands registered (SMATCHET_WITH_AGENTIC=OFF)"
    exit 0
fi

# H5 invariant — handoff.clarify must be present.
if ! echo "$LIST_OUT" | grep -q '"name":"handoff\.clarify"' ; then
    echo "[agentic-handoff-clarification] FAIL — handoff.clarify not registered"
    echo "$LIST_OUT" | head -3
    exit 1
fi

# Verify handoff.clarify surfaces a friendly error path. The proposal id 99999
# does not exist in the store, so the command must fail with a NON-empty error
# text — but must NOT crash or return a malformed envelope.
CLAR_OUT=$("$EXE" cmd handoff.clarify --proposal_id=99999 --answer=test_answer --spawn 2>&1 || true)
if ! echo "$CLAR_OUT" | grep -q '"command":"handoff\.clarify"' ; then
    echo "[agentic-handoff-clarification] FAIL — handoff.clarify envelope missing"
    echo "$CLAR_OUT" | head -5
    exit 1
fi
# The proposal doesn't exist → controller returns false with "no handoff for
# proposalId" (or "AgenticHandoffController not available" when PAT not set).
# Either error path is acceptable for the smoke — what matters is that the
# CLI returned ok=false cleanly.
if echo "$CLAR_OUT" | grep -q '"ok":true' ; then
    echo "[agentic-handoff-clarification] FAIL — handoff.clarify should not have succeeded against a non-existent proposal"
    echo "$CLAR_OUT" | head -5
    exit 1
fi

# Ping the dry-run path as a sanity check that the H5 extension didn't
# regress the H3/H4 wiring.
DRY_OUT=$("$EXE" cmd handoff.dry-run --proposal_id=99999 --use_stub_claude=true --spawn 2>&1 || true)
if ! echo "$DRY_OUT" | grep -q '"ok":true' ; then
    echo "[agentic-handoff-clarification] FAIL — handoff.dry-run regression"
    echo "$DRY_OUT" | head -5
    exit 1
fi

echo "[agentic-handoff-clarification] PASS — handoff.clarify registered + dry-run sanity ok"
exit 0
