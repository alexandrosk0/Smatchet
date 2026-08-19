# Plan — pre-push stage (B) refspec scoping
<!-- plan-date: 2026-08-19 -->

> **Slug**: `pre-push-refspec-scope`
>
> **Status**: `shipped` — [#2131](https://github.com/alexandrosk0/Smatchet/pull/2131), squash-merged `56f5be77` on 2026-08-19

## Context

`scripts/git-hooks/pre-push` stage (B) refuses any push made from a checkout whose
GitHub PR is `MERGED` / `CLOSED`. It decides that on the **checked-out branch name**
alone (`git rev-parse --abbrev-ref HEAD`), never looking at what is actually being
pushed. `agents/scripts/core/lock-release.sh:75` releases a plan-lock with
`git push origin ":refs/locks/<slug>"` — a bare ref **delete**, carrying no commits
and touching no branch ref — and that is exactly the push that happens right after a
PR merges, i.e. from a MERGED-PR checkout. So every routine lock release is refused
and needs `SMATCHET_ALLOW_MERGED_PR_PUSH=1`, training the operator to set the escape
hatch reflexively on a push the guard was never meant to cover.

Filed as `tooling/2026-08-17-pre-push-merged-pr-guard-is-refspec-blind.md` (P2), with a
recurrence on 2026-08-18 (PR #2111); this plan applies it, so the entry archives into
[`docs/self-improvement/categories/applied.md`](../../self-improvement/categories/applied.md).
After this lands, `lock-release.sh` runs clean from a merged-PR checkout and the
override stays reserved for what it names: commits pushed to a branch the PR will
never pick up.

## Approach

Give (B) the same per-update-record awareness stages (A) and (E) already have.
The hook already snapshots stdin once into `push_updates` (`:62`); (A) iterates it and
skips deletes (`:72`). (B) reads it not at all. Add the same loop to (B): refuse only
when at least one update targets `refs/heads/*` with a non-zero `local_sha`.

Empty or unparseable stdin stays on the **refuse** side. That is deliberate: it
preserves today's behaviour for any caller that drives the hook without records, and
it keeps the existing bucket-A harness honest — that harness runs the hook with empty
stdin, so a naive stdin-keyed guard would silently flip its MERGED/CLOSED refuse cases
green for the wrong reason. Fail-closed here costs nothing (a real `git push` always
supplies records) and removes the only way this change could weaken the guard.

## Files to modify

1. [`scripts/git-hooks/pre-push`](../../../scripts/git-hooks/pre-push) — stage (B):
   iterate `push_updates` before the `gh` lookup; `exit 0` when no update is a
   non-delete `refs/heads/*`. Header comment (`:15-26`) updated to state (B)'s scope.
2. [`agents/scripts/core/test-pre-push-merged-pr-guard.sh`](../../../agents/scripts/core/test-pre-push-merged-pr-guard.sh)
   — `run_hook` gains a stdin-records parameter (default: a branch content push, so
   the 9 existing cases keep asserting what they assert); three new cases.
3. `docs/self-improvement/categories/tooling/2026-08-17-pre-push-merged-pr-guard-is-refspec-blind.md`
   — the entry this plan applies; archived (with a § Status resolution) into
   [`applied.md`](../../self-improvement/categories/applied.md) and deleted, per
   `AGENTS.md` § Self-improvement loop.

## Existing utilities reused

- `push_updates` snapshot — `scripts/git-hooks/pre-push:62`; stdin is consumed once,
  every stage re-reads the variable via heredoc. No second `cat` needed.
- Stage (A)'s delete-skip idiom (`:72`, `[ "$local_sha" = "$zero_sha" ] && continue`)
  and its `zero_sha` constant — reused verbatim so both stages read identically.
- `run_hook` sandbox in `test-pre-push-merged-pr-guard.sh` (`env -i` + stub `gh` on a
  synthetic PATH) — extended, not replaced.

## Extraction sizing

N/A — no extraction or split; the diff adds ~15 lines to one shell stage.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — developer-machine git hook, not app code.
- **Pillar 2 (UI-thread)**: no impact — no UI code touched.
- **Pillar 3 (never crash)**: no impact — no product code touched.
- **Pillar 4 (accessibility)**: no impact — no UI surface.

## Perf-review-system gates

N/A — diff touches no `Source/Core/` file (shell hook + its bucket-A harness + docs).

## Risks / non-goals

- **Risk — a real merged-PR content push slips through.** Mitigated: the refuse path
  still fires for any non-delete `refs/heads/*` update, which is what an ordinary
  `git push` of commits produces; new bucket-A case asserts it.
- **Risk — empty stdin becomes an accidental bypass.** Mitigated by the explicit
  fail-closed branch (`parsed_updates -eq 0` → treat as a branch push) plus a case.
- **Non-goal**: no change to stages (A), (C), (D), (E); no new escape variable (so
  `test-pre-push-stage-neutralisers.sh` needs no new neutraliser); no change to when
  the guard fires for OPEN PRs or non-PR branches.

## Verification

- **Bucket A**: `bash agents/scripts/core/test-pre-push-merged-pr-guard.sh` — 9
  existing cases + 4 new (locks-delete from MERGED checkout → allow; branch content
  push from MERGED checkout → refuse; empty stdin from MERGED checkout → refuse;
  branch delete from MERGED checkout → allow).
- **Hermeticity lint**: `bash agents/scripts/core/test-pre-push-stage-neutralisers.sh --check`.
- **Bats**: `bats tests/bats/pre_push_guard.bats` — already drives the hook with real
  ref-update records; must stay green (its `gh` stub exits 0, so (B) is inert there).
- **Bucket E / screenshot / sanitizer**: N/A — no UI or C++ change.
- **Build gate**: N/A — pure shell + docs diff (`agents/scripts/core/is-pure-docs-diff.sh`
  will not classify it as docs-only, but no C++ TU is touched, so no rebuild is implied).
- **Doc validation**: `bash scripts/dev/test-docs.sh` — no NEW failure. Two failures
  are pre-existing and unrelated (a `test-agent-contract` DRIFT against the gitignored
  `.claude/` harness-adapter mirror, and `test-gate-selftests-bats` case 2 failing its
  own raw-self-exec fixture); both reproduce identically on a clean `origin/develop`
  worktree.
- **Full agentic suite**: `bash scripts/dev/test-all.sh` — `AGGREGATE  Passed: 2247
  Failed: 22  Scripts: 201`, no NEW failure. All 22 are pre-existing: every suite was
  re-run against a clean `origin/develop` worktree (`git worktree add --detach`) and
  reproduced identically —

  | Suite | Failure | Clean-tree result / cause |
  |---|---|---|
  | `test-adapter-drift` | 26 agent prompts DRIFT | gitignored `.claude/` mirror is locally stale; none of the 26 is in this diff |
  | `test-agent-contract` | 1 | `Passed: 27  Failed: 1` (same `.claude/` mirror) |
  | `test-archive-backlog-entry-bats` | case 16 | `Passed: 25  Failed: 1` |
  | `test-docs` | 2 | nested re-runs of `test-agent-contract` + `test-gate-selftests` |
  | `test-gate-selftests-bats` | case 2 | `Passed: 6  Failed: 1` |
  | `test-mutation-smoke` | corpus JSON validation | `Passed: 3  Failed: 1` (`Python was not found` — Windows App-Execution-Alias shim) |
  | `test-p4-mirror-bootstrap-bats` | cases 5, 7 | `Passed: 7  Failed: 2` |
  | `test-pre-push-format-delta-bats` | case 2 | `Passed: 1  Failed: 1` |
  | `test-review-guard-bats` | cases 14, 15 | `Passed: 14  Failed: 2` |
  | `test-shell-lint-bats` | case 16 | `Passed: 25  Failed: 1` |
  | `test-unwatched-pr-nudge-bats` | case 11 | `Passed: 15  Failed: 1` |
  | `test-verifier-endpoint-bats` | case 3 | `Passed: 5  Failed: 1` |
  | `test-verifier-preship-wiring-bats` | cases 3, 4, 5, 6, 9 | `Passed: 4  Failed: 5` |
  | `test-workflow-yaml` | `automated-pr-guard.yml`, `perf-pr-fast.yml` | `Passed: 30  Failed: 2` (`UnicodeDecodeError: 'charmap'` / `IndexError`) |

  `test-pre-push-format-delta-bats` is the only one that even touches this hook: its
  case 2 asserts a stage (D) refusal, and (D) runs before (B), so the (B) scoping
  cannot reach it — confirmed by the clean-tree run.
- **Shell lint**: `bash agents/scripts/project/test-shell-lint.sh` — `Passed: 343  Failed: 0`.
- **Lint gates**: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` — exit 0.
- **Plan stress-test — `grill-with-docs`**: run before finalising; outcome recorded in
  § Implementation log.

## Implementation log

Shipped in one slice as [#2131](https://github.com/alexandrosk0/Smatchet/pull/2131)
(`56f5be77`), two commits:

- `37827ffb` — the hook change plus the four new bucket-A cases.
- `c32ff736` — this plan's § Verification, recording the full-suite result.

(B) now iterates the `push_updates` stdin snapshot with the same idiom (A) uses at
`:69–83`, counting parsed records and setting `branch_content_push` only for a
`refs/heads/*` target with a non-zero `local_sha`. Zero parsed records falls through
to the refuse side. No new escape variable, so
`test-pre-push-stage-neutralisers.sh` needed no change.

**Live end-to-end proof.** Releasing this plan's own lock was the real test: a
`git push origin :refs/locks/pre-push-refspec-scope` issued from this branch's
checkout *after* #2131 merged — the exact scenario the guard was mis-firing on.
Run against the pre-fix hook it printed the refusal verbatim:

```
pre-push: REFUSING push.

  branch:    claude/pre-push-refspec-scope
  PR #2131 state: MERGED
```

Run against the fixed hook, with no `SMATCHET_ALLOW_MERGED_PR_PUSH` set, the delete
succeeded and `refs/locks/pre-push-refspec-scope` left `origin`.

**`core.hooksPath` is absolute — a hook fix is not live where you think it is.**
That A/B was only possible because the first attempt *failed*: this repo sets
`core.hooksPath=C:\Dev\Smatchet\scripts\git-hooks`, an absolute path into the
**main** checkout. Every worktree therefore runs whatever revision of the hook the
main checkout's branch happens to have — here `claude/peaceful-faraday-6jm1w5`,
which predates this fix — not the revision in the worktree's own tree, and not the
one on `develop`. The fixed hook had to be selected explicitly via a
`GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.hooksPath …` env override to run at all.
Filed as `tooling/2026-08-19-core-hookspath-absolute-serves-stale-hook-to-worktrees.md`.

**Post-ship follow-ups** (separate docs PR, this plan's archival slice):

- This plan moved `docs/plans/active/` → `docs/plans/shipped/`, and the
  `## Status` line in
  [`categories/applied.md`](../../self-improvement/categories/applied.md) was
  repointed at the new path — the link it carried
  (`../../plans/pre-push-refspec-scope.md`, tier-less) resolved to nothing even
  before the move. `docs/plans/shipped/` sits in the link checker's
  `EXCLUDED_PREFIXES`, so that dangling link was never going to be caught by a
  gate; it needed a human to look.
- The two `# Plan:` comments in
  [`test-pre-push-merged-pr-guard.sh`](../../../agents/scripts/core/test-pre-push-merged-pr-guard.sh)
  were tier-less for the same reason and are now pinned to `shipped/`, matching
  the `process-backlog-tighten` comment directly above them.
- Second backlog entry filed from the CR wedge this PR hit:
  `tooling/2026-08-18-cr-out-of-band-label-inert-until-gate-rerun.md` gained a
  third-occurrence section. #2131 isolated a **negative** result the earlier two
  did not — a bare `gh run rerun` with no label applied completes green and
  leaves the status `pending`. The recovery is the ordered pair *(label, then
  re-run)*, and both orderings fail silently.

## Deviations

None from the plan as written. Two additions made while implementing:

- A **fourth** bucket-A case (13: a `refs/heads/*` DELETE from a MERGED checkout →
  allow). The plan scoped three; deleting the merged branch post-merge pushes no
  commits either, so it belongs to the same equivalence class and was cheap to pin.
- The stage-(A) neutraliser comment in the harness was corrected in passing: it
  claimed (A) was "inert on the empty stdin here", which stopped being true once
  `run_hook` began feeding real ref-update records.

