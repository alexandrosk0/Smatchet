# Bugbot review rules — Smatchet

Project-specific guardrails for Cursor Bugbot. Bugbot loads this root
`.cursor/BUGBOT.md` on every PR. See `AGENTS.md` § Merge gates and
`docs/agent-rules/merge-gates.md` § Self-improvement doc PR auto-exemption.

## Skip the self-improvement ledger

- If a changed file is under `docs/self-improvement/**`, do NOT review it or post
  findings on it. That tree is the agent system's own self-improvement ledger
  (per-entry backlog files under `categories/`, `postmortems.md`, `applied.md`,
  `AGENT_SELF_IMPROVEMENT.md`) — never product code, never compiled. A pull
  request whose diff is **entirely** under `docs/self-improvement/**` should
  receive **no** Bugbot review at all.

The merge gate already treats such PRs as Bugbot-exempt
(`agents/scripts/core/merge-gates.sh`, `$selfImpOnly` — the hard guarantee that
they are never *blocked*). This file additionally asks Bugbot not to *spend* a
review on them, since Bugbot has no in-repo path-filter config of its own.
