# Force-push carve-out for spawned-agent API-500 recovery

# Status

**Withdrawn-as-partial (2026-05-21)** — the `agent/<id>` half of the carve-out is moot (the `ClaudeCodeLocalRunner` that produced that branch base was removed by v1 PR1 of [`docs/design/github-tracker-backend.md`](../design/github-tracker-backend.md), merge sha `b1d241bc`). The `claude/<id>` half — Claude Code SDK-spawned worktrees — remains valid and now stands alone in AGENTS.md § Project rules § Force-push carve-out for Claude Code SDK-spawned recovery (rewritten in this v2 cleanup commit). This ADR is kept for historical context of the original two-branch-shape reasoning; do not treat the `agent/<id>` carve-out below as live policy.

Originally: Accepted (2026-05-19).

# Context

The global `git push --force` ban in AGENTS.md § Project rules (and the harness's banned `--no-verify` / `--no-gpg-sign` flags) exists for a real reason: force-push silently overwrites public history, invalidates CodeRabbit approval (per merge-gates § STALE handling), and destroys parallel agents' pushed work on shared branches. It is a load-bearing safety rule.

However, Wave A2's 4/4 API-500 incidents (`tracker-labels`, `tracker-datetime`, `tracker-payload`, `tracker-field-catalog`) surfaced a recurring recovery shape: the agent's worktree has 100% of the intended file edits, but the agent's synthesis turn errored mid-run and never committed. The orchestrator recovers by inspecting the worktree, running local gates, `git add -A` + commit + push. One of the four (tracker-payload) committed before noticing that 3 new files weren't staged; recovery required `git commit --amend && git push --force-with-lease origin <branch>` to fold the missing files into the same commit (alternative: a noisy 2-commit history with the second commit titled "stage missed files").

The branches in question are spawned-agent worktree branches — `agent/<id>` (`ClaudeCodeLocalRunner`-spawned) and `claude/<id>` (Claude Code SDK-spawned). They share three invariants:

1. Single-owner. No other agent or human has push access to that branch during the run.
2. Pre-PR. The branch is either pre-PR-creation or carries an unmerged draft PR; no CI / CodeRabbit approval exists to be invalidated by amend+force.
3. Gitignored worktree. The local checkout lives under `.claude/worktrees/agent-<id>/` or `.claude/worktrees/claude/<id>/`; the user is not iterating in it.

Under those three invariants, `--force-with-lease` (which refuses the push if the remote ref changed since the local lease was captured) is observably safe: the worst case is the lease check fails (no destruction; the orchestrator falls back to a 2-commit history).

# Decision

Add one carve-out to the global force-push ban:

> `git push --force-with-lease origin agent/<id>` and `git push --force-with-lease origin claude/<id>` are permitted **only** during API-500 recovery (per `docs/agent-rules/DELEGATION.md` § API-500 mid-run recovery) when the orchestrator is amending an unpushed-since-API-500 commit on a spawned-agent worktree branch.

Explicitly excludes:

- `develop`, `main` — global ban remains absolute.
- `chore/*`, `feat/*`, `fix/*`, `docs/*`, `wip/*` — orchestrator-driven branches where other agents or humans may have pushed; non-API-500 force-push remains banned.
- Any branch where the ahead-range contains commits not authored by the current `gh api user --jq .login`. Mixed-authorship branches fall outside the spawned-agent single-owner invariant.

`--force-with-lease` is required; bare `--force` remains banned even within the carve-out.

# Consequences

- **API-500 recovery folds new-file commits into the original recovery commit.** No more split 2-commit history where the second commit just stages files the first commit missed. PR diffs read cleanly.
- **The carve-out is narrow and explicit.** Reviewers checking a force-push at PR time can verify the carve-out applies by inspecting the branch prefix + the merge-gates poll output (no other authors in ahead-range).
- **`--force-with-lease` is the only form permitted.** Bare `--force` would defeat the safety claim that "the worst case is the lease check fails". The lease check is what makes the carve-out load-bearing.
- **Trade-off: a future API-500 in a `chore/*` orchestrator branch cannot use this carve-out.** The orchestrator must do the 2-commit split or open a follow-up PR. That cost is acceptable because mixed-authorship `chore/*` branches happen at a much higher rate than orchestrator API-500 on a chore branch (the typical orchestrator chore PR commits incrementally and rarely amends).

# Alternatives considered

- **Always-new-commit recovery.** Reject every amend; force the orchestrator to make a follow-up commit titled "stage missed files". Pro: zero force-push policy. Con: every recovered PR carries a 2-commit history where the second commit's only diff is files that should have been in the first; reviewers ask "why split?" + the PR is noisier. Rejected because the recovery rate of Wave A2 (4/4 agents) suggests this is going to keep happening, and noisier PRs across that volume cost more than the narrow carve-out.

- **Full force-push ban + manual rescue.** Reject any in-orchestrator force-push; ask the user to do the amend manually. Pro: cleanest policy. Con: every API-500 becomes a manual human step; the orchestrator's autonomous-ship-loop value evaporates for any task that hits a transient API failure. Rejected because the carve-out's safety analysis (`--force-with-lease` + single-owner branch + pre-PR-approval) gives the same correctness as the manual rescue at zero human cost.

- **Carve-out for all `<your-login>/*` branches.** Broader scope: any branch the current user owns. Pro: covers `chore/*` and other orchestrator branches uniformly. Con: conflates the single-owner spawned-agent invariant with the multi-author orchestrator branch case where parallel agents do push. Rejected because the safety analysis breaks down outside the spawned-agent invariant.

# Cross-references

- Backlog: `docs/backlog/agent-self-improvement/process.md` (Wave A2 API-500 entry, archived to `applied.md` on Slice 5 ship).
- Recovery procedure: `docs/agent-rules/DELEGATION.md` § API-500 mid-run recovery.
- Plan: `docs/design/process-backlog-tighten-1-2-3-9-11-12.md` § Slice 5.
- Global ban: AGENTS.md § Project rules § Force-push carve-out for spawned-agent recovery (the rule this ADR records).
