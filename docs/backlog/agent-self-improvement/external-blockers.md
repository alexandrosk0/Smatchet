# Agent self-improvement — external-blockers

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Entries here cannot be resolved in this repo. Each names the upstream owner / workaround. Status: `blocked-external`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-18 · orchestrator · [external] · BLOCKED — Claude Code SDK spawns `claude/<id>` worktrees rooted on parent-repo current local HEAD, not `origin/develop`
  Owner: Claude Code SDK upstream.
  Details: Phase D + Phase E AI-assistant agents reported worktree branch HEAD rooted on `f2ce5b5` ("feat: add Google domain verification file") instead of develop. Each agent wasted ~3 min recovering via `git checkout -b <branch> origin/develop`. Root cause: the SDK's worktree-spawn machinery uses the parent repo's current local `HEAD` as the base — if parent is on an unrelated branch (`fix/<other>`, stale `develop`, sibling agent's branch), the new `claude/<id>` worktree inherits that base. Investigation of `git config --local` on the current worktree confirms `branch.claude/<id>.merge=refs/heads/develop` is set (so push/pull go to `develop`) but the initial commit base is whatever `HEAD` pointed at when the worktree was created. `extensions.worktreeconfig=true` is enabled but doesn't change the base-selection behaviour.
  Workaround / unblock: documented in `docs/harness/SETUP.md` § Worktree base — known stale-HEAD pitfall. Two tracks — (1) before opening a session, `git -C <repo-root> switch develop && git pull --ff-only`; (2) if a session is already running, first move is `git fetch origin develop && git rebase origin/develop` inside the worktree. Upstream fix: SDK should default base to `origin/develop` or expose `claude.worktree.baseBranch` config knob. Distinct from `ClaudeCodeLocalRunner` (`agent/<proposalId>` worktrees) which already bases on `origin/develop` via `handoff.auto_fetch_before_worktree` config flag default `true`.
  Status: blocked-external
  Last-reviewed: 2026-05-18

- 2026-05-16 · orchestrator · [external] · BLOCKED — Auto-merge disabled on the repo; `gh pr merge --auto` errors
  Owner: GitHub repository settings.
  Details: `gh pr merge <N> --squash --auto --delete-branch` errors with `GraphQL: Auto merge is not allowed for this repository (enablePullRequestAutoMerge)`. The autonomous-execution contract in `docs/design/applied/test-suite-expansion.md` § Auto-merge mechanics names `--auto` as the default. Orchestrator falls back to direct `gh pr merge --squash --delete-branch` after CI greens (poll wakeup every ~270 s — caches stay warm). Adds ~14 min wall-clock per PR vs `--auto`.
  Workaround / unblock: either (a) enable `enablePullRequestAutoMerge` at the repo level (one-time settings change, no agent edit) or (b) update `docs/design/applied/test-suite-expansion.md` § Auto-merge mechanics to document the direct-merge fallback + poll cadence. Estimated cost 1 min if (a) is chosen; 10 min doc edit if (b).
  Status: blocked-external
  Last-reviewed: 2026-05-17

- 2026-05-13 · orchestrator · [external] · BLOCKED — vexp `<!-- vexp -->` block auto-regenerates inside `AGENTS.md`; should land in `.claude/CLAUDE.md` instead
  Owner: vexp tool upstream.
  Details: AGENTS.md is the harness-agnostic root per the agents.md spec. The vexp tool injects ~30 lines of Claude-Code-specific MCP guidance (`run_pipeline`, `get_skeleton`, MCP tool list) directly into AGENTS.md, which other harnesses load and ignore. Editing the block in-place fights the regenerator. Upstream fix: vexp tool emits to `.claude/CLAUDE.md` only; the `@`-import in `.claude/CLAUDE.md` then pulls AGENTS.md without the vexp section.
  Workaround / unblock: file an issue / PR at the vexp project. Leave the block alone meanwhile; the ~250 input-token cost per session is small relative to the auto-regen friction of fighting it.
  Status: blocked-external
  Last-reviewed: 2026-05-17

- 2026-05-12 · tracker-backend · [external] · BLOCKED — `mcp__vexp__run_pipeline` rejects `max_tokens` as float when JSON wire format is double
  Owner: vexp tool upstream.
  Details: Surfaced as "floating point, expected usize" — schema should accept integers-as-floats or improve the error message.
  Workaround / unblock: file an issue / PR at the vexp project; not actionable in Smatchet. Cast to int literal in callers (`max_tokens: 12000` not `max_tokens: 12000.0`).
  Status: blocked-external
  Last-reviewed: 2026-05-17
