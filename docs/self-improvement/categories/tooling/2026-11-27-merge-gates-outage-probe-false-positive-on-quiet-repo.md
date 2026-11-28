# `merge-gates.sh` reports an Actions outage while a required check's job is still queued behind its parent

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-11-27
- **Observed on**: PR #2523 (merged 2026-11-27), three `safe-merge.sh` runs in one session.
- **Status**: open

## What happened

1. The first `safe-merge.sh` run ended `ESCALATE: ACTIONS UNAVAILABLE` (rc 7). Three required contexts (`Sanitizer (ASAN via MSVC)`, `Sanitizer (UBSan via Clang)`, `Bucket launch-smoke (Mesa GL)`) had no check run yet because they are jobs that `needs:` the still-running `Windows + MSVC` job. The probe's verdict was "3 required context(s) absent on head for 15 consecutive polls AND zero workflow runs created repo-wide since" the poll began. The head's own run had been created before that, and was still running, so a quiet repo read as GitHub Actions being down. `MERGE_GATES_OUTAGE_POLLS=60` only made the run wait longer before the same verdict.
2. Adding the `cr-out-of-band` and `cr-disposition:*` labels started duplicate workflow runs that GitHub cancelled at once. One cancelled run owned the `Test-delta gate` check, so `mergeStateStatus` stayed `BLOCKED` with every check green until that run was re-run by hand (`gh run rerun <id>`).
3. After re-running one failed job (a known flake), the aggregate `All checks green (block-on-any-red)` check stayed red from the first attempt and needed its own re-run.

## Why

The outage probe looks only at runs created since the poll began. It does not ask whether a run already in progress on the head will still create the missing checks, so it cannot tell "not created yet because the parent job has not finished" from "never going to be created". The gate poll also reads cancelled duplicate runs as final instead of looking for the newest run of each check.

## Proposed fix

- In the outage probe, before declaring an outage, list the head's workflow runs (`gh api repos/{o}/{r}/actions/runs?head_sha=<sha>`); if any is `in_progress` or `queued`, treat missing required contexts as pending.
- When `mergeStateStatus` is `BLOCKED` with all checks green, report the cancelled runs on the head by name and id, so the operator re-runs the right one instead of searching.
- Note in `merge-gates.md` that a job re-run leaves the aggregate check stale and needs its own re-run.

**Enumerator + replay**: PR #2523, head `03fee57107f4`: the first `safe-merge.sh` run returned rc 7 while the head's `build-and-test` run was still `in_progress`.
