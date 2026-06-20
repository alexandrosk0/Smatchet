- 2026-06-20 · claude-code · [process] · P2 — Out-of-band (GitHub-API) PR creation skips intent-capture → red `Intent section`

  Details: PR #1438 was opened via the GitHub MCP API (`create_pull_request`) on Claude Code on the web, where the local ship-loop intent-capture step (`docs/harness/claude-code/hooks/capture-intent.sh` → `.session-intent/<branch>.log` → templated `## Intent` in the PR body) never runs. The PR body therefore had no `## Intent` section, so the block-allowlisted `Intent section` doc-validation check went terminal RED. Same escaped class as the 2026-06-19 #1428 postmortem (red `Intent section` reaching `develop`) but a DISTINCT root cause: #1428 was a stale merge-watcher daemon running an out-of-date allow-list; #1438 is a PR-creation path that has no intent-capture hook at all.

  Concrete next action: Add an `## Intent` requirement to the out-of-band PR-creation contract in `docs/agent-rules/ship-loops.md` § Intent capture — any agent opening a PR via the GitHub API/MCP (i.e. with no local ship-loop) MUST hand-author a filled `## Intent` section in the PR `body` before calling `create_pull_request`. Optionally back it with a pre-create reminder in the harness PR-creation helper. Cheap (~30 min, doc rule).

  Status: open
  Last-reviewed: 2026-06-20
