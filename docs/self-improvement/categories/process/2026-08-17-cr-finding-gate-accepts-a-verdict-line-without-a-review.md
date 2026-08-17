# Every rung of the review-assurance ladder fails open

- **Category**: process
- **Priority**: P1
- **Date**: 2026-08-17
- **Observed on**: PR #2090 — `adversarial-code-review: 4 findings, all fixed` went green; an independent pass afterwards found 11, six of them defects in the diff
- **Status**: open

## What happened

PR #2090's review posture looked complete and was not. Three things combined:

1. The `adversarial-code-review:` line in the PR body was filled from a **self**-review conducted
   while authoring the diff. It was a real review — it found four genuine defects and they were
   fixed before push — but the author and the reviewer were the same pass over the same mental model.
2. **CodeRabbit had not reviewed it.** Its StatusContext reports `success` with
   `Review skipped: manual review required for this OSS repository`. A `success` that means "did
   nothing" is indistinguishable, to any consumer reading conclusions, from one that means "reviewed
   and found nothing". Note what this status does **not** mean: the review is **opt-in**, not
   unavailable — CodeRabbit requires a manual trigger on repositories with fewer than 10 stars, and
   `@coderabbitai review` obtains one on demand. Reading the skip as "no review is obtainable" is a
   second, separate error, and the author of this entry made it before re-reading the bot's own
   comment.
3. **Cursor Bugbot returned `neutral`** — also not a review, also not a failure.

So `CR finding gate` passed, and every visible signal said reviewed. An independent pass run later,
on the same diff, returned **11 findings — six of them defects in the change itself**, including two
regressions relative to base and one unsuppressable hard-FAIL shape
(`revisit=2026-roadmap` firing absolute `deviation-overdue`). None were subtle
enough to need luck: they were reachable by running the gate on eight one-line fixtures.

## Three rungs, all failing open

Chasing this on #2090 turned up not one weak check but a **ladder**, where every rung converts "not
reviewed" into something a reader scores as "reviewed, clean". Each rung was verified against the
source or observed live, not inferred.

**Rung 1 — the verdict line is a presence check.** `check-pr-intent.sh` / the `Intent section` job
regex-match `adversarial-code-review: N findings, …` and a `head=` binding. Nothing correlates that
string with a review artifact — not a run id, not a reviewer identity, not a finding count anyone else
produced. An author who self-reviews honestly and an author who types the line satisfy it identically.
The script says so itself: *"This proves a claim was RECORDED, never that the review ran."* The
head-binding half genuinely works — it invalidated the line on two pushes during this PR — but it
binds *when* the claim was made, not *whether* it is true.

**Rung 2 — the merge poller fast-passes the OSS skip.** `merge-gates.sh` field 23,
`cr_review_skipped`: true when the `CodeRabbit` StatusContext is SUCCESS with description
`Review skipped` and not the too-many-files size-skip. Its comment: *"A TERMINAL generic skip: CR
processed the PR and declined an incremental review (docs-only / path-filtered / trivial diff). The
NONE branch uses it to fast-pass."* So the skip is not merely ambiguous — it is **actively scored as a
pass**. The justification assumes the skip is *content*-based. In this repository it is **policy**-based:
CodeRabbit requires a manual trigger under 10 stars, so the identical skip lands on every PR regardless
of content. A 1030-line gate-logic change and a typo fix produce the same signal, and the fast-pass
reads both as "nothing worth reviewing" when they mean "nobody asked".

(Note for future readers: CodeRabbit's own recalled learning states merge-gates *"blocks on
review-skipped marker"*. That is inverted for the generic skip — `cr_size_skipped` is the hard block,
`cr_review_skipped` is the fast-pass. Read the script, not the learning.)

**Rung 3 — the opt-in path can fail while still looking pending.** Asking is possible, so this ladder
should end in a real review. On #2090 it did not: two invocations
(`8c278d40-fc8e-45c9-834b-840c70534e29`, `full review`, and `af5fbda5-9ea8-491d-8390-3dfea2f55af1`,
`review`) were both acknowledged and both returned only *"An error occurred during the review process."*
Worse, the first failure was published by **editing the acknowledgement comment in place** — a
collapsed `<details>Action performed — Full review triggered</details>` became
`<details>❌ Action failed — Review failed</details>` while the comment's visible first line still read
"Full review requested for #2090". The webhook is `issue_comment.edited`, which most watchers ignore.
A reader not diffing comment edits sees a review that looks in-flight, indefinitely.

So the honest coverage on #2090 was: one `/code-review` pass (11 findings, 6 real defects) plus the
author's self-review. **No external reviewer read it**, and three separate signals implied otherwise.

## Why it matters

This is the same fail-open shape #2090 exists to fix, one level up. `deviation-overdue` was green
because the parser could not see 57% of the markers; the review ladder is green because no rung can see
whether a review occurred. Both report on their own blind spot, and both fail in the reassuring
direction — and the review ladder is what let the deviation fix ship with six defects in it, two of
them regressions and one an unsuppressable repo-wide hard FAIL.

The cost is not hypothetical: it lands on whoever reads the PR next and prices "reviewed" from the
signals. On #2090 those signals were a green `CR finding gate`, a `CodeRabbit` context reading
`success`, and a Bugbot `neutral` — three greens over zero external review.

A separate mechanism, the `CR findings (0 actionable)` StatusContext posted by `cr-finding-gate.yml`,
blocks `merge-gates.sh` through the CI bucket when it sits pending (the pending count takes no
`$downgraded` subtraction, so no label reaches it). That was filed on `develop` via PR #2094 and is
**not** the same thing as `cr_review_skipped` above — do not conflate them. This entry is about the
review-assurance ladder.

## Concrete next action

1. **Distinguish "reviewed, clean" from "not reviewed".** Where the verdict line is parsed, require
   evidence naming what ran — a reviewer identity, or a tool plus run id. A self-review is legitimate
   and should stay allowed, but it should have to *say* it is one (`adversarial-code-review: self; N
   findings…`), so the next reader can price it correctly. Enumerator: every PR body on `develop`
   carrying an `adversarial-code-review:` line; today none records who reviewed.
2. **Stop fast-passing a policy skip.** `merge-gates.sh` should distinguish "CR declined *this diff*"
   from "the opt-in was never taken". The narrow fix is to exclude the
   `manual review required for this OSS repository` description from the `cr_review_skipped`
   fast-pass, so it routes through the NONE branch instead. The machinery to then obtain a review
   already exists and is switched off: `nudge_coderabbit()` posts `@coderabbitai review` once per
   HEAD, but the NONE-path counter defaults to **0 = DISABLED**. Setting it non-zero takes the opt-in
   automatically. Enumerator: `grep -n 'cr_review_skipped\|nudge_coderabbit' agents/scripts/core/merge-gates.sh`.
3. **Do not let a self-review satisfy the gate on a diff that changes gate logic.** A change to
   `lint-rules.d/**` or `agents/scripts/core/**` alters what every other gate can see, which is
   exactly where a blind spot is most expensive. Require a second pass for that path set.
4. **Treat a failed review invocation as a first-class state.** A `Review failed` that is published by
   editing a collapsed block in an older comment is indistinguishable from a review still running. If
   the merge poller consults CR at all, it should treat "requested and failed" as *not reviewed*
   rather than reading the acknowledgement as engagement.

Replaying the motivating case: #2090 touches `lint-rules.d/**`, so (3) would have required the
independent pass **before** push rather than hours after; (1) would have made the PR body say `self`
where it implied otherwise; (2) would have surfaced the missing review instead of fast-passing it; and
(4) would have caught that two requested reviews had already failed.

**Twice-corrected entry, and worth saying why.** The first draft claimed the OSS skip meant a review
was *unavailable*; the second claimed the skip *blocked* the merge poller. Both were wrong, and both
errors came from reasoning about `merge-gates.sh` and CodeRabbit's behaviour without opening either
source. That is precisely the failure mode the #2090 ledger records for its own verdicts — *an artifact
that exists is not an artifact you have read* — reproduced here by the person writing it down.

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-17;n=25; action=check whether any PR carrying an adversarial-code-review line records who reviewed, and whether a lint-rules.d change has shipped on a self-review alone; baseline=0 of N record a reviewer as of 2026-08-17; fired=never
