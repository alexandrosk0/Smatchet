- 2026-06-20 · claude-code · [tooling] · P3 — CCR-web remote has no non-`sleep` "merge when CI green" primitive (no `gh` in bash, `send_later` unavailable, webhooks don't deliver CI success)

  Details: Fulfilling "open a PR and merge it when green" from a CCR-web session, the merge-gates poller / merge-watcher (`agents/scripts/core/*`) aren't usable from the shell (no GitHub access in bash), `gh` is absent (must use `mcp__github__*`), the PR-activity subscription does **not** deliver CI-success or merge-conflict transitions (only failures + comments, per its own contract), and the recommended `send_later` self-check-in tool was not available in this session (ToolSearch returned only `Monitor`/`WebFetch`). The only way to catch "all checks terminal-green" was background `Bash sleep` timers re-polling via `mcp__github__pull_request_read` — exactly the busy-wait the remote-exec guidance says to avoid, but with no sanctioned alternative present. It worked (PR #1462 merged after verifying all 34 check-runs success/skipped), but it's fragile and against the documented pattern.

  Concrete next action: ensure `send_later` (claude-code-remote MCP) is available in CCR-web sessions, OR add an MCP "await PR mergeable / checks-complete" primitive, so an agent driving a merge request doesn't fall back to `sleep`-poll timers. Harness / CCR-image concern, not a Smatchet PR — filed as an agentic-infra learning.

  Status: open
  Last-reviewed: 2026-06-20
