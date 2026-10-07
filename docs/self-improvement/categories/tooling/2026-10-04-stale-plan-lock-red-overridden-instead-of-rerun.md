# A `Plan-lock gate` red is frozen at push time while the lock table it judged moves on, so a 12-day-old red got overridden instead of re-run

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-10-04
- **Observed on**: PR #2213. It merged 2026-09-24T13:48:26Z under `plan-lock-out-of-band`, crossing a `Plan-lock gate` red from 2026-09-12T00:46:43Z. A re-run at any time after 2026-09-12T17:22:38Z would have passed.
- **Status**: open

## What happened

`.github/workflows/plan-lock-gate.yml` runs on `pull_request: [opened, synchronize, reopened]` only. Its verdict depends on two inputs: the PR's diff, which is frozen between pushes, and the live `refs/locks/*` table, which is not. Locks get claimed and released, and `plan-lock-gate.sh:33` skips any lock older than its 14-day cutoff. Nothing re-runs the gate when the lock table changes, or when time alone ages a lock out.

#2213 (`chore: archive multi-jira plan and record #2212 merge snapshot`, branch `cursor/archive-multi-jira-plan-2eef`) had exactly one `Plan-lock gate` run, job `103468437213` at 2026-09-12T00:46:43Z:

```text
plan-lock-gate: 'docs/plans/INDEX.md' overlaps the write set of plan-lock
'gate-selftest-msys-execbit', held by a different branch.
```

That lock was **live** when the gate ran. Its branch `claude/stoic-mccarthy-082155` was the head of #2164, which was still open, and its write set lists `docs/plans/INDEX.md`. The red was correct when it was written. Then the inputs changed while the PR sat for 12 days:

| UTC | change |
|---|---|
| 2026-09-12 17:22:38 | the lock's `updated` (2026-08-29T17:22:38Z) passed the 14-day cutoff. From here on the gate would skip it |
| 2026-09-13 08:19:55 | `lock-staleness.yml` opened Issue #2215 "Stale plan-lock: gate-selftest-msys-execbit". It is still open, with 0 comments |
| 2026-09-13 21:24:01 | #2164 merged. Its body has no `lock-slug:` line, so the lock was orphaned, not released |
| 2026-09-24 11:36:31 | `plan-lock-out-of-band` applied to #2213. No disposition was recorded in the body or in a comment |
| 2026-09-24 13:48:26 | #2213 merged directly (no auto-merge request, no `merge-snapshots.jsonl` row) |

At the merge, the red was 12 days stale and its cause had expired twice over: the lock had aged past the cutoff, and its owner PR had merged. A re-run would have come back green, as `Plan-lock gate` did for #2243 (2026-09-24T17:12:44Z) and #2273 (2026-10-01), both of which edited `docs/plans/INDEX.md` while the same lock was still on origin. The override was load-bearing only because nobody re-ran the gate.

## Why it matters

- **A frozen red makes the hatch the path of least resistance.** The poller sees a terminal `failure` and lists the label as the way out, so the label gets used. A stale red and a real one look identical, and the waiver leaves no trace of which this was. This is the bare-boolean hatch from the #2160 entry, here used with no disposition at all.
- **It also hides the opposite case.** A `Plan-lock gate` green from an old push is equally stale if a conflicting lock was claimed after it. Today that green is trusted at merge.
- **The orphan is still there.** `refs/locks/gate-selftest-msys-execbit` is still on origin 21 days after #2164 merged. Past the cutoff it no longer blocks, but #2215 keeps being refreshed with no owner action. The staleness sweep has detected the orphan every day and done nothing about it, by design (Issue-only, never deletes).

## Concrete next action

1. **Re-evaluate a red `Plan-lock gate` at merge time, before honouring its override.** In `merge-gates.sh`, when the head's newest `Plan-lock gate` run is red:
   - get the PR's changed files (`gh pr diff <n> --name-only`);
   - `git fetch origin '+refs/locks/*:refs/locks/*'`;
   - pipe the file list through `plan_lock_gate_decide "<head ref>"`, sourced from `plan-lock-gate.sh`, against the live table.

   If it now comes back **clean**, the red is stale: refuse the `plan-lock-out-of-band` downgrade and print `gh run rerun <run id>`, the same way the duplicate-collapse WARN already prints its run lookup. If it is **still red**, the label applies, under the #2160 per-slug disposition rule.
2. **Mirror it for a stale green.** In the same block, a green `Plan-lock gate` whose completion predates the newest `refs/locks/*` update covering a changed file is re-checked the same way. A new collision blocks with the slug named.
3. **Re-run on lock changes (cheaper, partial).** Add `workflow_dispatch` plus a `schedule` (daily, after `lock-staleness.yml`) to `plan-lock-gate.yml` that re-runs the gate for every open PR whose latest run is red. This fixes the display but not the merge-time race, so (1) stays primary.
4. **Clear the live orphan** (operator action, needs approval): `bash agents/scripts/core/lock-release.sh gate-selftest-msys-execbit`, then close #2215 with a pointer to this entry. The structural fix for the orphaning is filed in [`2026-10-04-lock-release-on-close-keys-only-on-a-body-line.md`](2026-10-04-lock-release-on-close-keys-only-on-a-body-line.md), which covers #2164 as well as #2286.

**Enumerator + replay**: the enumerator has two parts:
- the newest `Plan-lock gate` check run on the PR head (`commits/{head}/check-runs?check_name=Plan-lock%20gate`), and the `overlaps the write set of plan-lock '<slug>'` lines it logged;
- `refs/locks/*` on origin at evaluation time.

Replayed on #2213 at 2026-09-24T13:48:26Z: the cutoff is 2026-09-10T13:48:26Z, and `gate-selftest-msys-execbit`'s epoch is 2026-08-29T17:22:38Z, which is older. `ltc_covering_slug docs/plans/INDEX.md other cursor/archive-multi-jira-plan-2eef <cutoff>` returns 1 (no covering lock), so the decision is clean. Action (1) refuses the label and prints the re-run for job `103468437213`. The re-run passes, and #2213 merges with no override.

The bats case goes in `tests/bats/merge_gates.bats`: a red `Plan-lock gate` fixture plus `plan-lock-out-of-band`, with a lock fixture older than the cutoff, must report `stale red — re-run` and must **not** downgrade.

Triggered-follow-up: when=pr-count:base=develop;since=2026-10-04;n=20; action=check whether merge-gates re-evaluates a red Plan-lock gate against the live lock table before honouring plan-lock-out-of-band, and whether refs/locks/gate-selftest-msys-execbit and Issue #2215 are gone; baseline=1 override of a 12-day-stale red (#2213) and 1 orphan lock 21 days past its PR's merge, 2026-10-04; fired=never

Re-scoped 2026-10-04 (backlog-sweep-2026-10): item 1 landed (backlog-sweep PR #2296: when plan-lock-out-of-band is in force the poller re-runs plan_lock_gate_decide against the current lock table; a clean result refuses the override as 'stale red — re-run'); item 4 is moot (the orphan lock is gone, #2215 closed). Remaining: item 2 (re-check a stale GREEN Plan-lock gate against locks claimed after it ran — needs a design decision: a diff + refs/locks fetch on every pass decision, and a new block on a green check with no escape) and item 3 (a scheduled/dispatch re-run in plan-lock-gate.yml).
