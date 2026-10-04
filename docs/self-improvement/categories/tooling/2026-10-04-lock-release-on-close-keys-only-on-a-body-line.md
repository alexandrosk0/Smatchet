# Plan-lock release-on-close keys only on a `lock-slug:` PR-body line, so a merged PR that omits it orphans its lock and forces an override on the next PR

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-10-04
- **Observed on**:
  - PR #2286 orphaned lock `crash-retire-terminate-prefs-enddisabled` when it merged 2026-10-04T01:59:46Z.
  - PR #2280 then merged at 04:47:49Z with `Plan-lock gate` red under `plan-lock-out-of-band`. The orphan was one of the two overlaps that label waived.
- **Status**: open

## What happened

`.github/workflows/lock-cleanup.yml` deletes `refs/locks/<slug>` on PR close only when the PR body contains a `lock-slug: <slug>` line. Otherwise it is a no-op, by design (header, `:3-10`). #2286's body had no such line. Its close-time run (`Release refs/locks/<slug> on PR close`, 01:59:52Z) logged `No 'lock-slug: <slug>' line found in PR body; no release.`

The lock stayed held by `fix/retire-pane-terminate-and-prefs-enddisabled`, a branch that had already merged and been deleted. Its write set included `tests/ui/tracker_first_run_setup.test.cpp`, the test #2286 added. That test was red on `develop`. The fix landed in #2280 (`f12f4bc2`, 03:25:40Z), and #2280's next `Plan-lock gate` run (03:29:58Z) failed on the orphan:

```text
plan-lock-gate: 'tests/ui/tracker_first_run_setup.test.cpp' overlaps the write set of plan-lock
'crash-retire-terminate-prefs-enddisabled', held by a different branch.
```

The only way through was the whole-gate `plan-lock-out-of-band` label (applied 03:29:33Z). That label also waived a second, unrelated overlap on the same run. `tests/CMakeLists.txt` was in the write set of the live lock `sanitizer-nightly-run-tests`, and no disposition mentioned it. See the follow-up on [`2026-09-12-plan-lock-out-of-band-waives-the-whole-gate-with-no-disposition-trail.md`](../process/2026-09-12-plan-lock-out-of-band-waives-the-whole-gate-with-no-disposition-trail.md).

The `lock-slug:` line is required by prose only. [`ship-loops.md`](../../../agent-rules/ship-loops.md) § Release wiring says the `open PR` step "MUST write a `lock-slug: <slug>` line". It also says that without it "the merged PR orphans its lock and Layers B/C false-block later overlapping PRs". This is that failure, and no gate stops a PR from opening, or merging, without the line. The only backstop is `lock-staleness-sweep.sh`, which has a 14-day cutoff and opens an Issue. It does not delete anything.

## Why it matters

An orphaned lock is a false gate. It blocks the PR that touches the file next, which is usually the one fixing a problem the lock's own PR shipped. That PR's only exit is a hatch that clears every overlap at once (#2160's open entry). So each orphan lowers the cost of waiving the gate, including for overlaps that are real.

The data needed to release the lock correctly is already in the lock itself. Every `refs/locks/<slug>` commit carries a `claim.json` with a `branch` field (`lock-claim.sh`, `LOCK_BRANCH`). The cleanup workflow ignores it.

## Concrete next action

1. **Release by branch match as well as by body line.** In `lock-cleanup.yml`, after the existing `lock-slug:` step, add a fallback step that runs on every close:
   - enumerate `GET repos/{repo}/git/matching-refs/locks/`;
   - read each ref's `claim.json` (`GET repos/{repo}/contents/claim.json?ref=<ref sha>`);
   - delete every lock whose `branch` equals `github.event.pull_request.head.ref`.

   Guards:
   - only when `pull_request.head.repo.full_name == github.repository` (a fork's branch name can collide);
   - never when the body carries `holds-lock:` (stacked intermediates must keep the shared lock);
   - a lock whose `branch` is `develop` or `main` is never matched. No PR head is an integration branch, and those locks are the edit-hook shims a worktree session has to claim under `develop`, because `.claude/hooks/guard-plan-lock.sh` takes the branch from the main checkout.
2. **Make the missing line visible before merge.** In `check-pr-intent.sh` and the `Intent section` job, WARN (not fail) when a live lock's `branch` equals the PR head and the body has no `lock-slug:` or `holds-lock:` line. Action (1) makes this cosmetic for the release, but the line remains the documented contract.

**Enumerator + replay**: the enumerator is `refs/locks/*` on origin, matched on `claim.json` `.branch` against the closed PR's `head.ref`.

- **#2286 closes (01:59:46Z).** Its head ref is `fix/retire-pane-terminate-and-prefs-enddisabled`. Lock `crash-retire-terminate-prefs-enddisabled` is held by that branch (#2280's 03:22:20Z comment names it). So action (1) deletes the lock at about 01:59:52Z, in the same run that logged the no-op.
- **#2280's 03:29Z `Plan-lock gate` run** then flags only `tests/CMakeLists.txt` against `sanitizer-nightly-run-tests`.

The bats companion lives in `tests/bats/` (or the lock primitives suite). Given a sandbox remote with two locks, branches `feat/x` and `feat/y`, closing a no-`lock-slug:` PR whose head is `feat/x` deletes only `feat/x`'s lock. Adding `holds-lock:` to the body deletes neither.

Triggered-follow-up: when=pr-count:base=develop;since=2026-10-04;n=20; action=check whether lock-cleanup.yml releases by claim.json branch, and count locks held by a branch whose PR is already closed; baseline=1 orphan (crash-retire-terminate-prefs-enddisabled, #2286) that forced a plan-lock-out-of-band override on #2280, 2026-10-04; fired=never
