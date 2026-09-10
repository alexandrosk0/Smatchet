# An orphaned plan-lock has no direct write path for a session-scoped token — release it through a closing PR's `lock-slug:` line instead

- **Category**: process
- **Priority**: P3
- **Date**: 2026-09-10
- **Observed on**: Issue #2182 (`refs/locks/fa-fetch-raw-host`), alongside #2183/`pillar2-shutdown-flush` (PR #2201's session) and #2198's `parent-issue-hierarchy` (same session)
- **Status**: applied (this PR)

## What happened

`refs/locks/fa-fetch-raw-host` was claimed 2026-08-17 by branch
`fix/fa-fetch-raw-host` for a one-line fix to
`.github/actions/fetch-fontawesome/action.yml`. The fix landed the next day —
folded into unrelated PR #2119 rather than shipped from the locked branch —
so `fix/fa-fetch-raw-host` was deleted with no PR ever pointing back at the
`fa-fetch-raw-host` slug. `lock-staleness.yml` flagged it 23 days later as
Issue #2182.

## Why the obvious fixes don't apply

- `lock-cleanup.yml` only fires on `pull_request: closed` and only releases a
  ref named by a `lock-slug:` line in *that* PR's body. A lock whose branch
  never became a PR — or whose fix shipped under an unrelated PR, as here —
  is orphaned forever by that path alone.
- `lock-release.sh` pushes a ref delete straight to `refs/locks/*`, which
  needs git credentials with write access to that namespace. A
  session-scoped `GITHUB_TOKEN` / CCR credential does not have it —
  confirmed by a repeatable HTTP 403 chasing the same problem for
  `parent-issue-hierarchy` (#2198's lock), released only because the repo
  owner ran the push locally with `SMATCHET_ALLOW_MERGED_PR_PUSH=1`.

## The fix that generalizes

`lock-cleanup.yml` releases on **any** PR close, merged or abandoned — so a
PR that carries `lock-slug: <slug>` in its body releases the ref the moment
it closes, whether or not its own diff touches the locked write-set at all.
PR #2195 used exactly this to release `pillar2-shutdown-flush` for #2183;
this PR does the same for `fa-fetch-raw-host` — the diff is this note, and
the ref is released by the `lock-slug:` line in the PR body, not by the
note's content.

Any future orphaned-lock Issue where the branch is gone and no PR names the
slug can be closed the same way: open (and merge or close) a PR whose body
carries `lock-slug: <slug>`. No push access to `refs/locks/*` required.
