# A PR-body rewrite deletes the `lock-slug:` marker and `Plan-lock cleanup` still reports success

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-17
- **Found during**: shipping [PR #2097](https://github.com/alexandrosk0/Smatchet/pull/2097) (`github-issue-body-empty-line`)

## Symptom

PR #2097 merged clean. The `Plan-lock cleanup` workflow ran, concluded **success**,
and `refs/locks/github-issue-body-empty-line` was **still live** afterwards — found
only because the closeout step happened to check. The run's only trace of the miss:

```
::notice::No 'lock-slug: <slug>' line found in PR body; no release.
```

Released by hand afterwards (`was bf0007015a6f65486449ff89561ae1e619907359`).

## Cause

The lock was claimed and the PR body *did* carry the bare `lock-slug:` line at PR-open
time. Later in the same ship-loop the `Intent section` CI check went red, and the fix
was a **full PR-body rewrite** (`gh pr edit --body`) adding the `## Intent` section and
a head-bound review verdict. The rewrite composed a fresh body and did not carry the
marker forward, so by merge time the string `lock-slug:` was absent entirely.

The [`lock-cleanup.yml`](../../../../.github/workflows/lock-cleanup.yml) parse step then
took its no-match branch — `::notice::` + `exit 0` + `slug=` — and the delete step,
gated on `steps.parse.outputs.slug != ''`, reported **skipped**. Green run, no release.

## Why this is not the entry already on file

[`2026-08-16-commented-lock-slug-marker-silently-skips-release.md`](2026-08-16-commented-lock-slug-marker-silently-skips-release.md)
covers the *commented-out* marker (`<!-- lock-slug: … -->` — present but unanchored),
and its proposed guard is explicitly keyed on that shape: *"when the body contains
`lock-slug:` but the anchored regex matched nothing"*. **That guard cannot fire here** —
the rewrite left no `lock-slug:` substring to detect. Same outcome (green run, stale
lock), disjoint detection surface. A fix for the commented case ships believing the
class is closed while this variant stays open.

The defect both share: the workflow reads the PR body as the **sole** record of the
claim, and a body is mutable by anything that edits it — CI-fix rewrites, template
churn, a bot. `refs/locks/<slug>` itself already records the claiming branch, so the
authoritative pairing exists outside the body and is never consulted.

## Proposed fix

1. **Stop treating the body as the claim record.** In the cleanup workflow, when the
   body yields no slug, query `refs/locks/*` for a lock whose recorded claimant branch
   equals `github.event.pull_request.head.ref`. A hit means the marker was lost, not
   absent: `::error::` and release (or fail loudly, naming the slug). This covers the
   commented, deleted, and never-written cases in one check, because it keys on the lock
   instead of on the prose. ~0.5d incl. a bats case alongside the existing `EXPECTED_PAT`
   assertions.
2. **Make the body-edit path marker-preserving.** Any ship-loop step that rewrites a PR
   body (Intent-gate remediation, verdict stamping) must re-read the existing body and
   carry `lock-slug:` / `holds-lock:` forward, or refuse. Cheapest form: a
   `gh pr edit --body` wrapper under `agents/scripts/core/` that diffs old→new for
   dropped marker lines.
3. **Assert at close, not days later.** The closeout sweep already knows the PR number;
   have it verify no `refs/locks/*` names this branch after merge, so the miss surfaces
   in the same turn instead of waiting for the 14-day staleness sweep.

## Why it matters

A stale lock is a live false-positive for every sibling session — Layer B of plan-lock
enforcement (`scripts/git-hooks/pre-push` stage (C)) refuses contended pushes against it,
so an unreleased lock blocks work on files nobody holds. And the signal shape is the worst
available: the gate whose entire job is releasing the lock reports **success** while
skipping the release — the "gate, don't trust" failure mode this repo files postmortems
over.
