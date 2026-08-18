# CR out-of-band review backfill

Retroactive CodeRabbit review of every PR that merged carrying `cr-out-of-band`
— the label that downgrades the CR merge-gate block to WARN
([`merge-gates.md`](../agent-rules/merge-gates.md) § Per-PR overrides). The label
waives the *gate*, never the *review*: each such merge left a diff on `develop`
that CR either never looked at or looked at and found something in. This ledger
drains that debt.

## Why a drip and not a sweep

CodeRabbit accepts a review request on a **merged** PR — it answers
`Full retroactive review requested for #<pr>` and reviews the merged head
(verified on #1977, 2026-08-16T23:37Z). What it will not do is accept them in
bulk: the account's plan carries **one included review at a time** on a rolling
window, so a second request inside the window returns

> Review rate limited. Your included review limit is currently reached … Your
> next included review will be available in N minutes.

Firing the whole queue at once therefore buys exactly one review and 89
rate-limit replies. The backfill instead **polls the quota every 15 minutes and
fires a single request per free slot** — roughly one PR per 45 minutes.

## Quota probe

The quota state is readable without spending a review: CR's reply to the most
recent request either confirms the review or carries the rate-limit warning with
the minutes remaining. `status: requested` + `outcome: rate-limited-retry` in the
ledger means the request bounced and the PR stays at the head of the queue.

## Ledger

`cr-oob-review-backfill.jsonl` — one row per owed PR, newest merge first.

| field | meaning |
| --- | --- |
| `pr` | PR number that merged with `cr-out-of-band` |
| `status` | `pending` → `requested` → `reviewed` (or `skipped`) |
| `requestedAt` | ISO-8601 stamp of the `@coderabbitai full review` comment |
| `outcome` | `rate-limited-retry`, `review-posted`, or a skip reason |

Enumerated from GitHub's label index
(`is:merged label:cr-out-of-band`, 90 PRs as of 2026-08-16). The label index is
lossy by design — `issue-sweep.sh` strips override labels post-merge, and the
lossless authority for anything it missed is `merge-snapshots.jsonl`
(`overrideLabels`). Rows the sweep adds from that source carry the same schema.

## Skips

Not every row owes a review. A **moot override** — the label pre-applied where
nothing was actually red (`overrideLabels` non-empty, `redChecks` empty; the
#1110 / #1124 class) — waived no review because none was pending, and resolves
to `status: skipped`. A PR whose merged head already carries a completed CR
review resolves the same way: the label waived the *findings*, not the review,
and re-reviewing it spends a slot the queue needs.
