# Plan — pre-push stage (B) refspec scoping

> **Slug**: `pre-push-refspec-scope`
>
> **Status**: `active`

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
  existing cases + 3 new (locks-delete from MERGED checkout → allow; branch content
  push from MERGED checkout → refuse; empty stdin from MERGED checkout → refuse).
- **Hermeticity lint**: `bash agents/scripts/core/test-pre-push-stage-neutralisers.sh --check`.
- **Bats**: `bats tests/bats/pre_push_guard.bats` — already drives the hook with real
  ref-update records; must stay green (its `gh` stub exits 0, so (B) is inert there).
- **Bucket E / screenshot / sanitizer**: N/A — no UI or C++ change.
- **Build gate**: N/A — pure shell + docs diff (`agents/scripts/core/is-pure-docs-diff.sh`
  will not classify it as docs-only, but no C++ TU is touched, so no rebuild is implied).
- **Doc validation**: `bash scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs`**: run before finalising; outcome recorded in
  § Implementation log.
