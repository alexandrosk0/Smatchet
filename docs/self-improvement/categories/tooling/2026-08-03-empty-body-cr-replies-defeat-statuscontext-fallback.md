# Empty-body CodeRabbit reply reviews defeat the CR gate's StatusContext fallback

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-03
- **Observed on**: PR #1928 (`feat/ui-thread-sync-reads`), head `0b077b5e`

## Friction

`CR findings (0 actionable)` — a **required** commit StatusContext — sat PENDING
for hours on a PR whose every other check was terminal-green, with no operator
action able to clear it.

`decide()` in [`.github/actions/cr-finding-gate/action.yml`](../../../../.github/actions/cr-finding-gate/action.yml)
branches on `n_reviews`, the count of CodeRabbit review nodes whose
`commit.oid == headRefOid`:

- `n_reviews == 0` → fall back to CodeRabbit's own `CodeRabbit` StatusContext;
  `SUCCESS` ⇒ pass.
- `n_reviews > 0` → grep the latest on-head review body for
  `Actionable comments posted: N`. A **missing header is fail-closed**
  (`return 1`, non-terminal, poll retries).

The wedge: replying to a CodeRabbit inline thread with
`addPullRequestReviewThreadReply` creates a **review node with an empty body**,
and CodeRabbit's auto-acknowledgement of that reply creates **another**. On
#1928 five thread replies produced three `coderabbitai[bot]` reviews on head
`0b077b5e` with `bodylen=0`.

That drove `n_reviews` from 0 to 3 — pushing the gate out of the branch where
the green `CodeRabbit` StatusContext would have passed it, and into the
header-grep branch, where three empty bodies can only ever fail closed. **The
act of responding to the review is what broke the gate.**

Worse, it is not self-healing on the same head. When CodeRabbit later posted a
genuine 6415-char review, every finding was an *"Outside diff range comment"* —
a body shape that carries **no** `Actionable comments posted:` header at all. So
the header grep still returned nothing and the gate still failed closed. Once a
head reaches this state the only exit is a **new head**.

Neither existing entry covers this: the
[adaptive-ratelimit](../applied.md)
one is about CR never *arriving*; the
[stuck-blockers](2026-07-13-cr-merge-gate-stuck-blockers.md) one is about
findings that *are* parseable. This is CR having arrived and the gate being
structurally unable to read it.

## Cost

~3 h of a session spent diagnosing and attempting recovery on an
otherwise-mergeable PR, ending in a no-op push purely to reset the head. Two
false starts along the way: a GraphQL review-body dump that returned empty
(needed the REST `repos/.../pulls/N/reviews` projection to reveal `bodylen=0`),
and a CR re-trigger whose gate run was then cancelled by concurrency with no
re-run.

## Proposed fix

Two independent changes, either of which unwedges this class:

1. **Ignore empty-body reviews in the `n_reviews` count.** A zero-length body
   carries no verdict, so it should not be evidence that CR reviewed this head.
   Filter `bodylen == 0` out before the branch, which restores the
   StatusContext fallback for the reply-only case.
2. **Treat a non-empty body with no actionable header as `0 actionable`, not as
   a retry.** The "Outside diff range comment" shape is a legitimate CR output
   with genuinely zero actionable in-diff comments. Fail-closed is right for a
   *truncated/unknown* body, but a body that parses as a complete CR review with
   no header is a pass, not an indefinite retry.

## Operator guidance until fixed

- **Do not reply to CodeRabbit threads via `addPullRequestReviewThreadReply`**
  while `CR findings (0 actionable)` is a required check. Address findings in
  the commit message on the fixing commit instead.
- If a head is already wedged, do not reach for `cr-out-of-band` on a code PR —
  push a new head. The label exists for a rate-limited CR, and using it here
  would wave un-reviewed code through.

## Status

Open.
