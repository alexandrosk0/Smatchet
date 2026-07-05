- 2026-07-05 · claude-code · [tooling] · P2 — plan-lock records the CURRENT branch; claiming from the wrong tree self-collides with your own push

  Details: `agents/scripts/core/lock-claim.sh <slug> <write-set>` stamps the lock's
  owner branch as **whatever branch the invoking tree is on** (`git rev-parse
  --abbrev-ref HEAD`). This session claimed `refs/locks/perf-win-hunt` from the MAIN
  repo tree (`/c/Development/Smatchet`, on `develop`) while the actual work + the
  push happened in a WORKTREE on `perf/win-hunt`. Result: the lock recorded
  `branch=develop`, and the pre-push plan-lock guard then rejected the
  `perf/win-hunt` push as a **collision with a DIFFERENT branch's write-set** — the
  agent colliding with its own lock. Recovery was a delete-ref + re-claim from the
  worktree (so `branch=perf/win-hunt`), plus a wasted push cycle.

  The confusing part: the lock and the branch are BOTH the operator's, so "plan-lock
  collision — overlaps the write set owned by a DIFFERENT branch" reads as if a
  second session is contending, when really it's a self-inflicted branch mismatch.

  Concrete next action (pick one):
  1. **Warn on tree/branch mismatch:** in `lock-claim.sh`, if the current branch is
     the repo's default/integration branch (`develop`/`main`) — an unlikely branch
     to hold a feature plan-lock — emit a loud "claiming lock owner=<branch>; you
     usually claim from the feature worktree, not the integration tree" note before
     the push. Cheapest, non-breaking.
  2. **Let the branch be explicit:** accept an optional `--branch <name>` (or
     `LOCK_CLAIM_BRANCH` env) so the caller pins the intended owner regardless of
     which tree runs the script — mirrors the worktree-per-session model.
  3. **Doc the gotcha** in `docs/perforce/AGENT_FLOWS.md` / the plan-lock section:
     "claim the lock from the SAME worktree that will push, so owner == pushing
     branch." (Do this regardless of 1/2.)

  Cross-ref: session PR #1632 (perf-win-hunt) — the lock claimed on `develop` blocked
  the `perf/win-hunt` push until released + re-claimed from the worktree.
