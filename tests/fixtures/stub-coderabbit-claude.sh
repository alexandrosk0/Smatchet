#!/usr/bin/env bash
# Synthetic stub — pretends to be `claude` (Claude Code CLI) for the
# coderabbit-react bucket-A smoke (`scripts/dev/test-coderabbit-react.sh`).
#
# Contract:
#   - Reads SEED.json from $PWD to discover the dispatch context. SEED.json
#     is written by `ClaudeCodeLocalRunner` before spawning. The presence of
#     SEED.json is the trigger; absence means we were invoked outside the
#     spawn flow and we exit cleanly without writing sentinels.
#   - Emits two stream-json NDJSON lines on stdout: one `assistant` event +
#     one `result` event. Matches the shape `ClaudeCodeLocalRunner` parses
#     (see `Source_Core/src/ClaudeCodeLocalRunner.cpp` event handlers).
#   - Writes `RUN_RESULT.json` + `PR_URL.txt` sentinels per
#     `AGENTS.md § Handoff envelope`.
#
# Mirrors the production C++ stub at `tests/fixtures/stub-claude/stub_claude.cpp`
# but in pure bash so the bucket-A smoke does not need a compiled binary
# to be present on PATH. Smatchet's runner accepts `claude` from PATH; the
# smoke shim sets PATH to a temp dir whose `claude` entry resolves here.
#
# Cleanup is the caller's responsibility (the smoke driver removes its
# temp dir on EXIT trap).

set -euo pipefail

# Best-effort cleanup of intermediate state if we abort mid-run. The
# sentinels we write are read by the runner / smoke driver after we exit,
# so do NOT remove them on a successful run — only on failure.
trap 'rc=$?; if [ "$rc" -ne 0 ]; then echo "[stub-coderabbit-claude] aborted rc=$rc" >&2; fi' EXIT

SEED_PATH="${PWD}/SEED.json"

if [ ! -f "$SEED_PATH" ]; then
    # No spawn context — caller is doing something other than the react
    # loop. Print a marker so the smoke driver can detect the missing
    # SEED.json case but exit cleanly.
    echo "[stub-coderabbit-claude] no SEED.json at ${PWD}; nothing to do" >&2
    exit 0
fi

# Emit canned stream-json. Two lines: one `assistant` event (the agent
# announcing the edit) + one `result` event (the terminal event that flips
# the runner FSM toward Complete). Real stream-json carries much more
# detail — we keep only the fields the runner's NDJSON parser consumes.
printf '%s\n' '{"type":"assistant","message":{"content":[{"type":"text","text":"Applying coderabbit-triage fix per SEED.json"}]}}'
printf '%s\n' '{"type":"result","subtype":"success","is_error":false,"total_cost_usd":0.0042,"usage":{"input_tokens":1234,"output_tokens":56}}'

# Sentinels. The runner watches for `RUN_RESULT.json`; `PR_URL.txt`
# mirrors `prUrl` for the UI's cheap-read path.
cat > "${PWD}/RUN_RESULT.json" <<'JSON'
{
  "ok": true,
  "errorMessage": "",
  "prUrl": "https://github.com/example/repo/pull/1234",
  "filesChanged": 1,
  "linesAdded": 5,
  "linesRemoved": 0,
  "toolUseSummary": "Edit×1, Bash×1"
}
JSON

printf '%s\n' 'https://github.com/example/repo/pull/1234' > "${PWD}/PR_URL.txt"

exit 0
