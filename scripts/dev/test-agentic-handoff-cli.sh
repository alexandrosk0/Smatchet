#!/usr/bin/env bash
# H4 — agentic handoff CLI smoke. Drives the new `handoff.*` commands
# end-to-end via the Smatchet exe. Skips cleanly when:
#   - The Smatchet exe is missing (e.g. ninja-iter-msys2 hasn't run yet).
#   - SMATCHET_WITH_AGENTIC=OFF — the handoff commands resolve to stub
#     handlers that always fail; the smoke can't distinguish that from a
#     real failure, so it skips.
#
# H4 surface checked:
#   1. `commands.list` contains every `handoff.*` command name.
#   2. `handoff.start --dry-run --proposal-id 99999` returns ok=true with the
#      computed branch (`agent/99999/...`) + worktree
#      (`.claude/worktrees/agent-99999`). Dry-run doesn't require a real
#      proposal to exist, so the smoke is hermetic.
#   3. `handoff.dry-run --proposal-id 99999 --use-stub-claude` returns the
#      same shape (developer convenience entry point).
#
# Future phases extend this script with real handoff lifecycle assertions
# (stub-claude end-to-end) once the proposal-insertion CLI is exposed.

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

# `commands.list` itself must succeed. Masking its exit code with `|| true`
# silently collapsed "binary is broken" and "AGENTIC=OFF, agent.* not listed"
# into the same SKIP path, hiding real regressions. Capture stdout+stderr +
# the exit code separately so we can FAIL on a genuinely broken exe.
if ! LIST_OUT=$("$EXE" cmd commands.list --spawn 2>&1); then
    echo "[agentic-handoff-cli] FAIL — commands.list exited non-zero"
    echo "$LIST_OUT" | head -10
    exit 1
fi

# AGENTIC=OFF gates every handoff.* command to a stub that always fails. The
# command names still register so commands.list lists them, but exercising
# them returns errors not worth distinguishing in this smoke — skip cleanly.
# Whitespace-tolerant match survives `--pretty`-style JSON output.
if ! echo "$LIST_OUT" | grep -qE '"name"[[:space:]]*:[[:space:]]*"agent\.triage\.run"' ; then
    echo "[agentic-handoff-cli] SKIP — no agentic commands registered (SMATCHET_WITH_AGENTIC=OFF)"
    exit 0
fi

# H4 invariant — every handoff.* command name must be present. Whitespace-
# tolerant: `"name"\s*:\s*"<name>"` so the test survives `--pretty` output.
for NAME in handoff.start handoff.cancel handoff.list handoff.clarify handoff.dry-run; do
    if ! echo "$LIST_OUT" | grep -qE "\"name\"[[:space:]]*:[[:space:]]*\"${NAME//./\\.}\"" ; then
        echo "[agentic-handoff-cli] FAIL — handoff command not registered: $NAME"
        echo "$LIST_OUT" | head -3
        exit 1
    fi
done

# Drive handoff.start --dry-run. Dry-run is hermetic: no proposal row needed,
# no runner spawned. The reply must report the canonical branch + worktree.
# The CLI accepts `--name=value` for typed params (see cmd commands.help).
START_OUT=$("$EXE" cmd handoff.start --proposal_id=99999 --dry_run=true --spawn 2>&1 || true)
if ! echo "$START_OUT" | grep -qE '"command"[[:space:]]*:[[:space:]]*"handoff\.start"' ; then
    echo "[agentic-handoff-cli] FAIL — handoff.start envelope missing"
    echo "$START_OUT" | head -5
    exit 1
fi
if ! echo "$START_OUT" | grep -qE '"ok"[[:space:]]*:[[:space:]]*true' ; then
    echo "[agentic-handoff-cli] FAIL — handoff.start dry-run did not return ok=true"
    echo "$START_OUT" | head -5
    exit 1
fi
if ! echo "$START_OUT" | grep -qE '"worktree"[[:space:]]*:[[:space:]]*"\.claude/worktrees/agent-99999"' ; then
    echo "[agentic-handoff-cli] FAIL — handoff.start dry-run worktree shape unexpected"
    echo "$START_OUT" | head -5
    exit 1
fi
if ! echo "$START_OUT" | grep -qE '"branch"[[:space:]]*:[[:space:]]*"agent/99999/' ; then
    echo "[agentic-handoff-cli] FAIL — handoff.start dry-run branch shape unexpected"
    echo "$START_OUT" | head -5
    exit 1
fi

# Drive handoff.dry-run as a sanity check on the dedicated stub-claude entry.
DRY_OUT=$("$EXE" cmd handoff.dry-run --proposal_id=99999 --use_stub_claude=true --spawn 2>&1 || true)
if ! echo "$DRY_OUT" | grep -qE '"command"[[:space:]]*:[[:space:]]*"handoff\.dry-run"' ; then
    echo "[agentic-handoff-cli] FAIL — handoff.dry-run envelope missing"
    echo "$DRY_OUT" | head -5
    exit 1
fi
if ! echo "$DRY_OUT" | grep -qE '"ok"[[:space:]]*:[[:space:]]*true' ; then
    echo "[agentic-handoff-cli] FAIL — handoff.dry-run did not return ok=true"
    echo "$DRY_OUT" | head -5
    exit 1
fi
if ! echo "$DRY_OUT" | grep -qE '"use_stub_claude"[[:space:]]*:[[:space:]]*true' ; then
    echo "[agentic-handoff-cli] FAIL — handoff.dry-run did not echo use_stub_claude"
    echo "$DRY_OUT" | head -5
    exit 1
fi

echo "[agentic-handoff-cli] PASS — handoff.* commands registered + dry-run round-trip ok"
exit 0
