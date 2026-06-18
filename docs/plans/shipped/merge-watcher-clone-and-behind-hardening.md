# Plan — Merge-watcher clone-key + BEHIND-advance hardening (Cluster B)

> **Slug**: `merge-watcher-clone-and-behind-hardening` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — merged via PR #1393 (squash `5267b4569841`).

## Context

Cluster B of the 2026-06-18 self-improvement sweep — the two today-P2 merge-watcher entries:

1. `merge-watcher-register-keyed-by-cwd-clone` (`tooling.md` :28) — `register` keys each entry by the clone PATH it is invoked from, so registering a PR from an agent worktree (`.claude/worktrees/<slug>`) binds the entry to that ephemeral clone, producing a duplicate/orphan vs the stable main clone and leaving the daemon polling a path that vanishes at teardown.
2. `merge-watcher-no-update-branch-for-standalone-behind-pr` (`infra.md` :223) — a green + CR-cleared REGISTERED PR that drifts BEHIND base on a busy develop is never auto-advanced: the daemon's only `update-branch` path is the post-merge cascade for stacked CHILDREN, so a standalone BEHIND PR starves until a manual `gh pr update-branch` (lived on PR #1358, BLOCKED ~24h twice).

Intended outcome: a worktree registration de-dupes onto the stable main clone automatically, and (opt-in) the daemon auto-advances a registered PR whose ONLY blocker is BEHIND, bounded so a hot develop can't churn.

## Approach

**Entry 1** — canonicalize the clone path in `resolve_clone_path()` via `git rev-parse --git-common-dir`: its parent dir is the PRIMARY checkout root (absolute from a worktree, relative `.git` from the main clone). A linked worktree thus de-dupes onto the main clone; a genuinely separate full clone has its own common-dir and stays distinct. Transparent in a normal (non-worktree) checkout, where `show-toplevel == canonical`.

**Entry 2** — add `maybe_auto_update_behind()` dispatched from `maybe_escalate_stuck_pr()` when `_classify_pr_wedge` returns `BEHIND`. Opt-in via `MERGE_WATCH_AUTO_UPDATE_BEHIND=true` (off by default). Dispatches the existing `cascade_update_child()` server-side `update-branch` ONLY when the Poll line parsed AND fail/pending/req-missing/cr-open are all 0 (green + CR-cleared). Bounded by an atomic reserve mirroring `_atomic_reserve_auto_act`: single dispatch per `(PR, head_sha)` (a successful update advances the head, restarting the key) and a per-PR-lifetime `MERGE_WATCH_AUTO_UPDATE_BUDGET` (default 2). When disabled / not-green / dedup'd / out-of-budget it returns `{}` so the normal STUCK escalation (human notify) still fires. The `BEHIND` STUCK reason text now names `gh pr update-branch` / `safe-admin-merge.sh <pr>` / the new knob as operator remedies (the no-code interim from the entry).

## Files to modify

1. `agents/scripts/core/merge-watcher-cli.py` — `resolve_clone_path()` canonicalization (Entry 1).
2. `agents/scripts/core/merge-watcher.py` — `_atomic_reserve_auto_update()` + `maybe_auto_update_behind()` + wiring in `maybe_escalate_stuck_pr()` + `_STUCK_REASON_TEXT["BEHIND"]` (Entry 2).
3. `tests/bats/merge_watcher.bats` — register-canonicalization CASE + 2 auto-update-behind CASES.

## Existing utilities reused

- `cascade_update_child()` — `merge-watcher.py:833` — the server-side `update-branch` dispatch (no local checkout, no force-push); reused verbatim for the standalone-BEHIND path.
- `_atomic_reserve_auto_act()` — `merge-watcher.py:1249` — the dedup+budget reserve pattern mirrored by the new `_atomic_reserve_auto_update`.
- `_parse_poll_ci_counts()` / `_classify_pr_wedge()` — the green-gate + BEHIND classification, reused.

## UX Pillar callouts

- **Pillar 1–3**: no impact — daemon/CLI Python, not product UI-thread code.
- **Pillar 4**: N/A — no UI.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else N/A)

N/A — diff touches only `agents/scripts/core/*.py` + `tests/bats/**`; no `Source/Core/`.

## Risks / non-goals

- **Risk: update-branch churn on a hot develop.** Mitigated — opt-in (default off) + per-(PR,head) dedup + per-PR-lifetime budget (default 2); over-budget falls through to the human STUCK escalation.
- **Risk: auto-advancing a not-actually-clean PR.** Mitigated — dispatch only when the Poll line PARSED and fail/pending/req-missing/cr-open are all 0; an unparseable line never dispatches.
- **Risk: canonicalization changes the stored clone_path.** CI-safe — in a normal checkout `show-toplevel == canonical`; only a linked-worktree registration changes (de-dup, the intended fix). The `merge_watcher.bats` suite conflates `REPO_ROOT` as both script-path and clone_path, so it must run from a primary clone (CI does); verified green there.
- **Non-goal**: the older merge-watcher robustness entries (daemon backstop, unbounded `triage_attempts`, autostart, agent-notify channel) — separate follow-ups.

## Verification

- **Bucket A**: N/A — covered by bats.
- **Bash-driver**: `bats tests/bats/merge_watcher.bats` — register-canonicalization + 2 auto-update-behind CASES green; full suite green from a PRIMARY clone (CI condition; verified via a throwaway plain-repo harness). `py_compile` clean on both Python files.
- **Build gate**: N/A — no C++.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test before finalising; record outcome.
- **Manual residue**: none — bats + py_compile are deterministic.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray refs to deferred items before finalising.

- `merge_watcher.bats` `REPO_ROOT` script-path/clone-path conflation — a latent test-design coupling exposed (not introduced) by canonicalization; refactoring it to a separate `CLONE_PATH` var is a follow-up, filed if it recurs.
- The remaining 7 Cluster-B robustness entries — separate.

## Implementation log
- PR #1393 (squash `5267b4569841`) — `merge-watcher-cli.py`: `resolve_clone_path()` canonicalizes a linked-worktree cwd onto its primary clone via `git rev-parse --git-common-dir` (`:28`). `merge-watcher.py`: `_atomic_reserve_auto_update` + `maybe_auto_update_behind` dispatched from `maybe_escalate_stuck_pr` on a `BEHIND` wedge, opt-in `MERGE_WATCH_AUTO_UPDATE_BEHIND`, green+CR-cleared-only, dedup-per-head + `MERGE_WATCH_AUTO_UPDATE_BUDGET` (default 2); `BEHIND` STUCK reason-text names the operator remedies (`:223`). `merge_watcher.bats` +3 CASES.
- Follow-up PR #1403 — decoupled the `merge_watcher.bats` `REPO_ROOT` conflation (`SCRIPTS_DIR` worktree vs `CLONE_PATH` canonical) so the suite passes from a linked worktree; also fixed a stale `NOTIFY_STATES` assertion + added a `_count_live_sessions` stub. (Resolves the `tooling.md` `merge-watcher-bats-repo-root-conflates-script-path-and-clone-path` debt this plan flagged in § Out-of-scope.)

## Deviations from plan
- CodeRabbit caught a failure-path contract bug in `maybe_auto_update_behind` (non-empty return on `gh repo view`/dispatch failure suppressed the human STUCK escalation); fixed so EVERY non-acting path returns `{}` — only a successful dispatch returns the delta. Cursor added the `User`-count to the green gate and a registry-streak clear; both applied pre-merge.

## Verification (actual)
- `merge_watcher.bats` register-canonicalization + 2 auto-update-behind CASES green from a primary clone (CI condition; verified via a throwaway plain-repo harness); `py_compile` clean; `scripts/dev/test-docs.sh` 13/13. The suite's `REPO_ROOT` script-path/clone-path conflation (flagged in § Out-of-scope) was the only worktree-only false-fail — resolved in follow-up #1403.
