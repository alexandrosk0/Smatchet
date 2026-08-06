# CR finding gate wedges on an empty-body CodeRabbit review

- **Category**: process
- **Priority**: P1
- **Date**: 2026-08-05
- **Observed on**: PR #1948 (`fix/fa-ttf-worktree-fallback`)

## What happened

CodeRabbit posted a `COMMENTED` review node on the PR head
(`e56ac352`) with an **empty body** — its acknowledgement after the one inline
finding was addressed and the thread resolved. No new push followed, so CR never
posted another review.

`.github/actions/cr-finding-gate/action.yml` then took its fail-closed branch:

- `n_reviews > 0` (a review node exists on this head), so the
  `n_reviews == 0` disambiguation via CR's own `CodeRabbit` StatusContext —
  which *was* `SUCCESS` — is never reached;
- the review body carries no `Actionable comments posted: N` header, so
  `n` is empty and `decide()` returns non-terminal;
- the 12×15 s window exhausts and the action posts
  `pending — awaiting CodeRabbit review on current head`.

`CR findings (0 actionable)` is a **required** StatusContext, so the PR sat at
`mergeStateStatus=BLOCKED` with every other check green. Re-running the workflow
re-ran the identical logic and re-posted PENDING — the state is not
self-healing; only a new push or a fresh CR review can clear it, and neither is
guaranteed to arrive.

Unwedged by applying `cr-out-of-band` (user-authorised, "ignore cr") and
re-running the gate, which took the label-override early-exit. That is an
override label standing in for a gate that could not reach a verdict — the shape
we normally treat as a gate escape.

## Why it matters

The fail-closed branch is correct in spirit (a header-less review is not proof
of "0 actionable"), but it has no terminal state for the case where the
header-less review is CR's *final* word on the head. Every such PR needs a human
override, which erodes the label's meaning as a deliberate exception.

## Proposed fix

In the `n_reviews > 0` / empty-`n` path, disambiguate the same way the
`n_reviews == 0` path already does, but only for a **body-less** review:

- if the latest on-head CR review body is empty/whitespace **and** CR's own
  `CodeRabbit` StatusContext on that head is `SUCCESS`, treat it as
  "CR settled with nothing actionable" → `post success`;
- keep the current non-terminal retry for a **non-empty** body that merely lacks
  the header (that really is an unsettled/unexpected state).

An empty body cannot hide a finding count, so this does not reopen the #524
fail-open (which was a *preamble line above* the header, i.e. a non-empty body).

Add a case to whichever harness covers the action's decision table so the
empty-body + StatusContext-SUCCESS combination is pinned.
