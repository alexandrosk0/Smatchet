# CR out-of-band review backfill

Retroactive CodeRabbit review of every PR that merged carrying `cr-out-of-band`
— the label that downgrades the CR merge-gate block to WARN
([`merge-gates.md`](../agent-rules/merge-gates.md) § Per-PR overrides). The label
waives the *gate*, never the *review*: each such merge left a diff on `develop`
that CR either never looked at or looked at and found something in. This is the
drain for that debt.

## Why a drip and not a sweep

The account's plan carries **one included review at a time** on a rolling window.
A request made inside that window is answered with

> Review rate limited. Your included review limit is currently reached … Your
> next included review will be available in N minutes.

and is **dropped, not queued** — #1977's rate-limited request of 2026-08-16 had
still produced no review 39 hours later. A bounced request must be retried; it
must never be assumed pending. Firing the whole queue at once therefore buys at
most one review and 89 rate-limit replies.

So the backfill polls every 15 minutes and fires **at most one** request per open
window — roughly one PR per 45 minutes, days of wall-clock for a 90-PR queue.

## Why it runs in CI, not in a session

The first cut ran as a session-scoped 15-minute cron. The session was reclaimed
and the drip made **zero progress in 39 hours**. That is the same class
[`unwatched-pr-nudge.sh`](../../agents/scripts/core/unwatched-pr-nudge.sh) was
written for: a check-in that was supposed to post a review trigger once CR's
quota reopened, and never did. A drain measured in days cannot live in a process
measured in hours.

[`cr-oob-review-backfill.yml`](../../.github/workflows/cr-oob-review-backfill.yml)
runs the poller on a `*/15`-equivalent schedule;
[`cr-oob-review-backfill.sh`](../../agents/scripts/core/cr-oob-review-backfill.sh)
holds the logic.

## Stateless by design

No mutable state is kept. A status column written by the process that dies cannot
report that the process died — so every question is asked of GitHub instead:

| question | answered by |
| --- | --- |
| has PR N been requested? | N carries a `cr-oob-backfill:` marker comment |
| did the request land? | CR's reply under that marker |
| does N still owe a review? | N's merged head carries no CR review |

A run recomputes from scratch and is safe to interrupt, re-run, or overlap. It
needs no repo write access — `pull-requests: write` for one comment is the whole
surface.

`cr-oob-review-backfill.jsonl` is therefore a **static queue**, not a ledger: one
`{"pr":N}` row per owed PR, newest merge first. Enumerated from GitHub's label
index (`is:merged label:cr-out-of-band`, 90 PRs as of 2026-08-16). That index is
lossy by design — `issue-sweep.sh` strips override labels post-merge — so
`merge-snapshots.jsonl` `overrideLabels` stays the lossless authority for
anything it missed.

## The unconfirmed-request fail-safe

CR acknowledged a retroactive request on a merged PR once (`Full retroactive
review requested for #1977`, 2026-08-16) and then, on a retry 39 hours later,
answered nothing at all — no reply, no review. **Whether CR ever DELIVERS a
retroactive review is therefore unconfirmed**; the acknowledgement is not proof.

That uncertainty is load-bearing: if a request can silently no-op, then
*requested* is not *reviewed*, and a drip that only counted comments posted would
work through all 90 PRs, collect nothing, and leave the debt looking paid. So the
poller **halts** when its most recent request has gone unanswered past the
confirmation grace (default 30 min) and says so, rather than marching on. Any CR
response clears it — including a rate-limit reply, because the question is
whether CR is answering at all, not whether the review passed.

## What the silence is not

The obvious reading — that merged PRs are the problem — is **wrong**, and a
control ruled it out. On 2026-08-18 the repo's own CR finding gate posted
`@coderabbitai review` on OPEN PR #2119 sixteen minutes after the merged-PR
request; both drew the same silence, as had its nudges on open #2087 and #2084
(~40h earlier, still no review). CR is unresponsive across the repo, open and
merged alike, so the blocker is CR-side capacity — plausibly the adaptive limit
its [plans docs](https://docs.coderabbit.ai/management/plans#rate-limits)
describe for sustained high-volume activity.

So a halt is **not** a signal to re-plumb the collection route. It resolves on
its own when CR answers again, and the poller resumes without intervention. Only
if CR is demonstrably answering on open PRs while still ignoring merged ones does
the merged-PR route need replacing (re-opening each diff on a fresh PR) — and
that is a human call, not something the drip should decide.

This has a sharper consequence than a stalled backfill: while CR is silent,
CURRENT PRs merge unreviewed too, so the queue grows faster than any drip drains
it. Fixing the source outranks draining the debt.

## The 2026-08-18 duplicate-comment incident

The first live deployment posted **six identical review requests to #1995** in
two hours — one per scheduled tick — and would have kept going. It is the exact
failure the fail-safe above was written to prevent, and it happened anyway.

**Cause.** The request record was read with
`gh search issues "<marker> in:comments"` — and **three independent guards all
read from that one call**: "has this PR been requested", the quota window (via
the newest request timestamp), and the unanswered-request fail-safe. When it
returns empty, it does not degrade one guard; it silences all three at once, and
the poller re-picks the same newest PR every tick.

It returns empty because **GitHub's search index is eventually consistent**. The
first diagnosis written here was that the marker is inside an HTML comment and
therefore never indexed. That was wrong, and the live workflow disproved it: the
23:03Z tick printed `HOLD window closed for 1066 more second(s)`, which is only
reachable when the search *does* return the marker. The index simply lags the
write by a long time — and the whole storm fits inside that lag. So the storm was
bounded (six comments, ~2h) rather than unbounded, and it stopped on its own when
the index caught up.

That correction sharpens the lesson rather than softening it: **an idempotency
guard must never be built on an eventually-consistent index.** The window in
which such an index reports "nothing has happened yet" is precisely the window in
which the guard is being asked whether something already happened. A slow source
fails exactly like a broken one.

A second flaw in the same query: it matches **any** issue mentioning the marker
string, including PRs that merely *discuss* the backfill (#2119 and #2126 both
do). Those false positives skew `last_req`, wedging or unwedging the quota window
for reasons unrelated to any actual request.

**Why the tests missed it.** `--selftest` and the bats suite both inject the
decision input directly through `SMATCHET_CR_BACKFILL_FIXTURE`. They proved the
decision core was correct — and it *was* correct, on every tick. What no test
covered was the layer that fills that input, so a core that reasons perfectly
over an empty world passed everything while being catastrophically wrong.

**Fixes, in order of how much they are trusted.**

1. **Per-PR request record.** The marker is read from each PR's own comments —
   the same place `post_request` writes it — scanned newest-first, stopping at
   the first PR without one. No shared, silently-empty lookup.
2. **A failed lookup HOLDS.** `scanfail` is distinct from "no marker": an errored
   or unparseable response means the state is *unknown*, and unknown is never a
   licence to post. Treating a failed read as "nothing requested" is the whole
   incident in one sentence.
3. **Last-line idempotency.** `post_request` re-reads the PR immediately before
   writing and refuses if the marker is already there. Deliberately redundant
   with (1) and (2), because a duplicate is never correct no matter what the
   decision core concluded, and the blast radius of getting it wrong is
   unbounded.

**A second instance of the same class, caught in review** (Bugbot on #2126): the
first cut of the per-PR scan read only the first page of issue comments. That
endpoint returns oldest-first and a backfill marker is by construction the
newest comment, so on any PR with more than 100 comments the marker would be
invisible to *both* the scan and the pre-post check — the identical shared
failure, reintroduced one page-size assumption later. Both lookups now paginate.
The lesson generalises: a guard is only as good as the completeness of the read
underneath it, and "I fixed the shared-source bug" is not the same as "no shared
assumption remains".

**The schedule is disabled** until a manual `workflow_dispatch` with
`dry_run: true` shows the poller holding on #1995 against live GitHub. The data
layer cannot be exercised from the authoring container (no `gh`), and that gap
is precisely what shipped the bug — so it is closed by observation, not by
another green selftest.

## Skips

Not every row owes a review. A **moot override** — the label pre-applied where
nothing was red (`overrideLabels` non-empty, `redChecks` empty; the #1110 / #1124
class) — waived no review because none was pending. A merged head that already
carries a completed CR review is the same: the label waived the *findings*, not
the review, and re-requesting spends a slot the queue needs. The poller re-checks
this at fire time, since the queue is static but review state is not.
