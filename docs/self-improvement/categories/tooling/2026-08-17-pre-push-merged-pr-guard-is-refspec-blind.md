# pre-push (B) is refspec-blind: it refuses `refs/locks/*` deletes from a merged branch

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-08-17
- **Found during**: releasing the plan-lock after [PR #2097](https://github.com/alexandrosk0/Smatchet/pull/2097) merged

## Symptom

The documented release path, run from the worktree of the branch whose PR had just
merged:

```bash
bash agents/scripts/core/lock-release.sh github-issue-body-empty-line
```

refused with the merged-PR banner:

```
pre-push: REFUSING push.
  branch:    claude/github-issue-body-empty-line-9aa2f2
  PR #2097 state: MERGED
Pushing to a MERGED PR branch silently lands commits the PR will never pick up.
```

Nothing was being pushed to the PR branch. `lock-release.sh:75` pushes a **delete**:
`git push "$remote" ":$ref"` where `$ref` is `refs/locks/<slug>`. Cleared with
`SMATCHET_ALLOW_MERGED_PR_PUSH=1` — an override the hook's own header labels *"rare,
usually wrong"*, on the one path the ship-loop is supposed to take every time.

## Cause

Stage (B) of [`scripts/git-hooks/pre-push`](../../../../scripts/git-hooks/pre-push)
never looks at what is being pushed. It keys entirely on the checkout:

- `:170` — `branch=$(git rev-parse --abbrev-ref HEAD)`
- `:366` — `gh pr view "$branch" --json state`; `exit 0` only when empty or `OPEN`
- `:373-396` — otherwise print the banner and `exit 1`

The `push_updates` snapshot taken at `:62` — which carries `<local_ref> <local_sha>
<remote_ref> <remote_sha>` for every update — is read by stage (A) and stage (E) but
not by (B). Both of those stages already recognise a delete and skip it: (A) at `:72`
(`[ "$local_sha" = "$zero_sha" ] && continue   # a branch DELETE — not a content push`),
(E) by exempting deletes per its header at `:36`. (B) is the only stage that judges the
push without reading it, so *every* refspec inherits the merged-PR refusal — lock
deletes, tag pushes, any sibling ref — on the sole basis of which branch happens to be
checked out.

The guard's own justification does not extend to these: the banner's premise is
"commits the PR will never pick up", and a `refs/locks/*` delete carries no commits and
touches no branch ref.

## Proposed fix

Give (B) the same delete/ref awareness (A) and (E) already have: iterate `push_updates`
and only refuse when at least one update targets `refs/heads/*` with a non-zero
`local_sha`. That is a handful of lines and it preserves the guard's entire purpose (the
orphaned-commit case) while removing the false refusal for lock deletes and other refs.

One trap in the test harness, worth naming because it decides the default: the existing
bucket-A harness
[`agents/scripts/core/test-pre-push-merged-pr-guard.sh`](../../../../agents/scripts/core/test-pre-push-merged-pr-guard.sh)
(9 cases) runs the hook with **empty stdin** — its own comments note (A) "is stdin-driven
and inert on the empty stdin here". A naive stdin-keyed (B) would therefore see zero
updates and allow, flipping the harness's MERGED/CLOSED refusal cases (6, 7) green for
the wrong reason. So: keep empty/unparseable stdin on the **refuse** side (fail-closed,
matching today's behaviour), teach `run_hook` to pipe ref-update records, and add two
cases — a `refs/locks/<slug>` delete from a MERGED-PR checkout is allowed, a
`refs/heads/<branch>` content push from the same checkout still refuses.

## Why it matters

The stale-lock class this compounds is filed separately
([`2026-08-17-pr-body-rewrite-drops-lock-slug-marker.md`](2026-08-17-pr-body-rewrite-drops-lock-slug-marker.md)),
and the two chain: the automated release silently no-ops, and then the documented manual
recovery is blocked by a hook that tells the operator they are almost certainly doing
something wrong. The cost is not the extra env var — it is that reaching for
`SMATCHET_ALLOW_MERGED_PR_PUSH=1` on a routine, correct operation is exactly how an
override stops meaning anything.

## Recurrence

- **2026-08-18** — same refusal releasing the `fix-four-open-issues` lock after
  [PR #2111](https://github.com/alexandrosk0/Smatchet/pull/2111) merged; cleared the same
  way (`SMATCHET_ALLOW_MERGED_PR_PUSH=1 bash agents/scripts/core/lock-release.sh
  fix-four-open-issues` → `refs/locks/fix-four-open-issues deleted`). Second occurrence in
  two days, both on the routine post-merge release path — the override is now the *normal*
  way to run `lock-release.sh`, which is the failure mode this entry predicted. Priority
  unchanged at P2 (loud refusal with a documented escape, not a silent failure); the
  frequency is the argument for scheduling the § Proposed fix rather than for a bump.
