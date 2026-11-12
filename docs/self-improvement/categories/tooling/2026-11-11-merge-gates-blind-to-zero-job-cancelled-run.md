# The merge gate cannot see a required workflow's run that was cancelled before any job started, so GitHub stays BLOCKED with every gate green

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-11-11
- **Observed on**: PR #2402 (head `f7c41fab5c`). `merge-gates.sh` and `safe-admin-merge.sh` both reported every gating check green, while GitHub's `mergeStateStatus` stayed `BLOCKED` for hours.
- **Status**: open

## What happened

The `Perf PR-fast` workflow ran three times for the head's `pull_request` event on 2026-11-09. One run (42677019167) was cancelled by its concurrency group before any job started. It left a completed/cancelled check suite with zero check runs. The other two runs reported `Perf PR-fast (windows-2022)` as `skipped`.

The gate reads check runs, so the zero-job run is invisible to it:

- `merge-gates.sh` printed `GATES_PASSED`, with CI at 22/22. Its duplicate check-name WARN named only `Independent review (enforcement surface)`, a non-required check whose newer run was green.
- `safe-admin-merge.sh` printed `GREEN … genuine stale-BLOCKED carve-out`. Then `gh pr merge --admin` failed with `Required status check "Perf PR-fast (windows-2022)" is expected`. That was the first message that named the blocker.
- All 23 required contexts were reported on the head and passing or skipped. There were no rulesets, threads or required reviews.

Re-running 42677019167 created its jobs: `Detect` succeeded and `Perf PR-fast` was skipped. `mergeStateStatus` went to `CLEAN`, and the armed auto-merge landed the PR within seconds. Earlier re-runs of the other two Perf runs had not cleared it.

## Concrete next action

In `merge-gates.sh`, when GitHub reports `BLOCKED` and every gate passes, list the head's check suites over REST (`commits/<sha>/check-suites`). Flag any `github-actions` suite that is `completed` with `conclusion: cancelled` and `latest_check_runs_count: 0`. Resolve its run id from `actions/runs?check_suite_id=<id>` and print `gh run rerun <run-id>` in the BLOCK line, beside the duplicate check-name WARN. Add the zero-job shape to `docs/agent-rules/merge-gates.md` § Duplicate check-name divergence, with a bats fixture in `tests/bats/merge_gates.bats`.

## Second shape: suites stuck `queued` with zero runs (PRs #2401, #2408, #2412)

On 2026-11-11T20:34Z, `labeled` events on #2401, #2408 and #2412 each created eight `github-actions` check suites that never started: `status: queued`, `latest_check_runs_count: 0`. They belonged to the label-triggered workflows (`Perf PR-fast`, `Coverage gate (test-delta)`, `CR finding gate`, `All checks green`).

- GitHub refused `gh run cancel` and `--force` on all of them with HTTP 409 `Cannot cancel a workflow run that has not been queued yet`, and a queued run cannot be re-run.
- `mergeStateStatus` stayed `BLOCKED` with every required context green on the head. `safe-admin-merge.sh` printed `GREEN … genuine stale-BLOCKED carve-out`, then `gh pr merge --admin` failed with `3 of 23 required status checks are expected`.
- The only route found was a new head. #2412 got one from a push, and #2401 and #2408 from `gh pr update-branch`; the normal gate poll then merged each PR.

The next action above should cover this shape too: flag a `queued` suite with zero runs that is older than a few minutes, and print `gh pr update-branch <pr>` rather than `gh run rerun`, since a run that never started can be neither cancelled nor re-run.
