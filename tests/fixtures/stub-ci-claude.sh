#!/usr/bin/env bash
# Synthetic stub — pretends to be `claude` (Claude Code CLI) for the
# ci-react bucket-A smoke (`scripts/dev/test-ci-react.sh`).
#
# Contract:
#   - Reads SEED.json from $PWD (dispatch context).
#   - Reads CHECK_RUN.json from $PWD if present (the CI-failure payload
#     that the ci-react dispatch path writes alongside SEED.json — phase
#     6 + phase 7 of `docs/design/coderabbit-react-loop.md`). When the file
#     is missing the stub still completes successfully and prints a marker
#     so the smoke driver knows.
#   - Emits canned stream-json simulating a `build-doctor` fix path: an
#     `assistant` event + a synthetic `cmake --build` exit-0 announcement
#     embedded in the assistant text + a terminal `result` event.
#   - Writes `RUN_RESULT.json` + `PR_URL.txt` per `AGENTS.md § Handoff
#     envelope`.
#
# Mirrors the production C++ stub at `tests/fixtures/stub-claude/stub_claude.cpp`
# but in pure bash — see `stub-coderabbit-claude.sh` for the broader
# rationale (no compiled-binary dependency for the bucket-A smoke).

set -euo pipefail

trap 'rc=$?; if [ "$rc" -ne 0 ]; then echo "[stub-ci-claude] aborted rc=$rc" >&2; fi' EXIT

SEED_PATH="${PWD}/SEED.json"
CHECK_RUN_PATH="${PWD}/CHECK_RUN.json"

if [ ! -f "$SEED_PATH" ]; then
    echo "[stub-ci-claude] no SEED.json at ${PWD}; nothing to do" >&2
    exit 0
fi

if [ ! -f "$CHECK_RUN_PATH" ]; then
    # Phase-6 invariant: ci-react dispatch always writes CHECK_RUN.json
    # alongside SEED.json before the spawn. Missing == driver bug.
    echo "[stub-ci-claude] WARN: CHECK_RUN.json absent at ${PWD} (ci-react dispatch is expected to write it)" >&2
fi

# Canned stream-json — simulate a `build-doctor` fix that re-ran cmake
# successfully (exit 0). The runner's parser reads the `type` discriminator
# and ignores the body of `assistant` events; the `result` event is the
# terminal signal.
printf '%s\n' '{"type":"assistant","message":{"content":[{"type":"text","text":"Re-running cmake --build --preset ninja-iter-msys2 after fix..."}]}}'
printf '%s\n' '{"type":"assistant","message":{"content":[{"type":"text","text":"cmake --build exited 0; CI failure fingerprint resolved."}]}}'
printf '%s\n' '{"type":"result","subtype":"success","is_error":false,"total_cost_usd":0.0061,"usage":{"input_tokens":1789,"output_tokens":92}}'

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
