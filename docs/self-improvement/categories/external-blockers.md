# Agent self-improvement — external-blockers

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Entries here cannot be resolved in this repo. Each names the upstream owner / workaround. Status: `blocked-external`.

<!-- Latest first. Append new entries at the top. -->

- 2026-06-18 · orchestrator · [external] · BLOCKED — `PLAN_INDEX_PAT` is configured but its push 403s, so the `Auto-sync plan INDEX` job's auto-fix is dead and every `docs/plans/` PR must hand-commit `INDEX.md`
  Owner: GitHub repo admin (alexandrosk0/Smatchet secrets + PAT scope).
  Details: the `doc-validation.yml` `Auto-sync plan INDEX (PR branch)` job regenerates `docs/plans/INDEX.md` and, on drift, commits + pushes it via `PLAN_INDEX_PAT` (added per applied.md's `plan-index-pat` entry to re-enable the auto-fix convenience). On #1394 the job logged `PAT_CONFIGURED: true`, regenerated (+2 plans), committed `chore(plan): auto-sync shipped-plan INDEX`, then failed: `remote: Permission to alexandrosk0/Smatchet.git denied to alexandrosk0` → `403` on push → fell back to the fail-loud `exit 1` path. So the PAT EXISTS but lacks push permission (fine-grained PAT not granted `Contents: Read and write`, or not authorised for this repo / its branches). Net: the convenience is non-functional and the author must manually `--fix` + commit `INDEX.md` on every plan-archival / plan-moving PR (a recurring required-check red — #953 historically, #1394 now). Two reds per occurrence (`Auto-sync plan INDEX` + `Doc anchors + agent contract`, same INDEX-drift root).
  Workaround / unblock: re-issue the `PLAN_INDEX_PAT` repo secret as a fine-grained PAT with `Contents: Read and write` on alexandrosk0/Smatchet (confirm it can push to PR head branches), or switch the job to a GitHub App installation token. Until fixed: the plan-archival playbook must say the INDEX auto-sync WILL 403 and the author commits `INDEX.md` themselves (regenerated FROM the worktree root — see tooling `plan-index-fix-wrong-cwd-silent-noop`). Resolvable only in GitHub repo/secret settings.
  Status: blocked-external
  Last-reviewed: 2026-06-18

- 2026-05-18 · orchestrator · [external] · BLOCKED — Claude Code SDK spawns `claude/<id>` worktrees rooted on parent-repo current local HEAD, not `origin/develop`
  Owner: Claude Code SDK upstream.
  Details: Phase D + Phase E AI-assistant agents reported worktree branch HEAD rooted on `f2ce5b5` ("feat: add Google domain verification file") instead of develop. Each agent wasted ~3 min recovering via `git checkout -b <branch> origin/develop`. Root cause: the SDK's worktree-spawn machinery uses the parent repo's current local `HEAD` as the base — if parent is on an unrelated branch (`fix/<other>`, stale `develop`, sibling agent's branch), the new `claude/<id>` worktree inherits that base. Investigation of `git config --local` on the current worktree confirms `branch.claude/<id>.merge=refs/heads/develop` is set (so push/pull go to `develop`) but the initial commit base is whatever `HEAD` pointed at when the worktree was created. `extensions.worktreeconfig=true` is enabled but doesn't change the base-selection behaviour.
  Workaround / unblock: documented in `docs/harness/SETUP.md` § Worktree base — known stale-HEAD pitfall. Two tracks — (1) before opening a session, `git -C <repo-root> switch develop && git pull --ff-only`; (2) if a session is already running, first move is `git fetch origin develop && git rebase origin/develop` inside the worktree. Upstream fix: SDK should default base to `origin/develop` or expose `claude.worktree.baseBranch` config knob. Distinct from `ClaudeCodeLocalRunner` (`agent/<proposalId>` worktrees) which already bases on `origin/develop` via `handoff.auto_fetch_before_worktree` config flag default `true`.
  Status: blocked-external
  Last-reviewed: 2026-05-18

