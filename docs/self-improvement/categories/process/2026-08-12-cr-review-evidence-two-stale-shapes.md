- 2026-08-12 · claude-code · [process] · P2 — a CodeRabbit review that COMPLETED can still leave the head with no review evidence the `CR findings` gate accepts, in two observed shapes: a rate-limit-stale `success` status, and a comment-only clean pass that posts no review object; both are indistinguishable from "never reviewed" until something re-triggers the reviewer

  Details: the gate rule (shipped after #1996, tightened by the ledger learning
  of 2026-08-11) is correct: `state: success` + description `Review rate
  limited` is NOT review evidence, and the gate must see an actual review on
  the CURRENT head. What this entry records is how often a genuinely-completed
  review still fails to produce that evidence, measured across the #1999 merge
  drive (2026-08-12, ~7 review rounds):

  1. **Rate-limit-stale status.** The auto-review attempt posts `success /
     "Review rate limited"` and never updates, even after a later
     comment-triggered review of the same head completes clean. Observed on
     head e33b5ca0: the 08:11 incremental pass replied "Review complete — no
     actionable findings" as an ISSUE COMMENT, posted no review object, and
     left the 07:13 rate-limit status in place; the gate re-polled at 08:22
     and correctly reported "awaiting CodeRabbit review on current head".
     Correct gate, wedged PR.
  2. **Comment-only clean pass.** `@coderabbitai review` on an
     incrementally-clean head can complete without submitting a GitHub review
     object at all (its reply carries the verdict as prose). Nothing for the
     gate's GraphQL query to find; same wedge from a different door.

  Both resolved the same way both times: `@coderabbitai full review`, which
  always submits a review object and refreshes the commit status ("It must
  create current-head review evidence" — CodeRabbit's own ack of the request).
  Cost when it recurs: one full extra review cycle plus however much of the
  adaptive rate-limit window the retry burns (25-55 min per wait, four waits
  during the #1999 drive).

  Also worth recording for the next long merge drive: pushing to a PR while
  CodeRabbit is mid-review ABORTS the review ("head commit changed during the
  review"), and the automatic retry burns the next rate-limit slot — the
  costly half of the #1999 churn was self-inflicted by exactly that. Batch
  fixes; push once; request once.

  Concrete next action: teach the `CR finding gate` workflow's poller the
  distinction it already half-knows. When it observes (a) a CodeRabbit status
  whose description is terminal-but-evidence-free (`Review rate limited`, or
  `Review completed` with no review object on the head) AND (b) a completed
  clean pass advertised only in comments, it should POST the
  `@coderabbitai full review` nudge itself — once per head, budget-capped —
  instead of parking on `pending` until a human or a timer intervenes. The
  merge-gates.sh side already has the auto-post shape
  (MERGE_GATES_STALE_REREVIEW_POLLS); the CI gate lacks it. Est ~0.5d
  including bats coverage for the once-per-head cap.

  Status: open
  Last-reviewed: 2026-08-12
