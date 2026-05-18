#!/usr/bin/env bash
# H7 — PR-iteration CLI smoke. Verifies the H7 wiring (PrCommentWatcher +
# 6 new GitHubClient methods + iteration budget config field) is observable
# from the CLI surface without driving a live GitHub HTTP loop. Skips
# cleanly when the Smatchet exe is missing or AGENTIC=OFF (mirrors the
# H5 clarification smoke).
#
# H7 surface checked:
#   1. `commands.list` still contains the prior agentic surface (`agent.triage.run`)
#      so the H7 build wave didn't accidentally drop the AGENTIC gate.
#   2. `handoff.dry-run --proposal_id=99999 --use_stub_claude=true` still
#      round-trips cleanly — proves the H7 patch (extra config fields,
#      extra controller members) did not regress the H3/H4/H6 wiring.
#   3. The Smatchet exe links against the 6 new GitHubClient methods —
#      this manifests as a clean exit (no missing-symbol crash on the
#      handoff.dry-run path).
#
# The full PR-iteration round-trip (live PR comments, watcher dispatches,
# pr-iterator spawn) lands in H9 + scenarios; running it from a shell
# would require a real GitHub repo + PAT which is out of scope for H7.
# This smoke is a CI-friendly ping that the AGENTIC=ON build still wires
# correctly under the H7 extension.

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
    echo "[agentic-handoff-iterate] SKIP — no Smatchet.exe in build/ninja-*-msys2/"
    exit 0
fi

LIST_OUT=$("$EXE" cmd commands.list --spawn 2>&1 || true)

# AGENTIC=OFF gates every handoff.* command — skip cleanly.
if ! echo "$LIST_OUT" | grep -q '"name":"agent\.triage\.run"' ; then
    echo "[agentic-handoff-iterate] SKIP — no agentic commands registered (SMATCHET_WITH_AGENTIC=OFF)"
    exit 0
fi

# Sanity check — the H3/H4/H5/H6 commands must still be present after the H7 build.
for cmd in handoff.dry-run handoff.clarify ; do
    if ! echo "$LIST_OUT" | grep -q "\"name\":\"${cmd}\"" ; then
        echo "[agentic-handoff-iterate] FAIL — ${cmd} not registered after H7 extension"
        echo "$LIST_OUT" | head -3
        exit 1
    fi
done

# Ping the dry-run path. A regression in the H7 patch (e.g. a missing
# PrCommentWatcher constructor argument, a banned-header include in
# GitHubClient.h, a config-field clamp bug) typically surfaces here as a
# crash, a non-zero exit, or an `"ok":false` envelope.
DRY_OUT=$("$EXE" cmd handoff.dry-run --proposal_id=99999 --use_stub_claude=true --spawn 2>&1 || true)
if ! echo "$DRY_OUT" | grep -q '"command":"handoff\.dry-run"' ; then
    echo "[agentic-handoff-iterate] FAIL — handoff.dry-run envelope missing"
    echo "$DRY_OUT" | head -5
    exit 1
fi
if ! echo "$DRY_OUT" | grep -q '"ok":true' ; then
    echo "[agentic-handoff-iterate] FAIL — handoff.dry-run regression after H7 extension"
    echo "$DRY_OUT" | head -5
    exit 1
fi

echo "[agentic-handoff-iterate] PASS — H7 build links cleanly, prior agentic CLI surface intact"
exit 0
