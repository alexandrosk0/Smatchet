# The `CR finding gate` verifies a verdict line exists, not that a review happened

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

## Why it matters

The gate is a **presence** check on a string the author writes. Nothing correlates that string with
a review artifact — not a run id, not a reviewer identity, not a finding count anyone else produced.
An author who self-reviews honestly and an author who types the line satisfy it identically, and the
PR-body verdict then reads as third-party assurance to the next reader.

This is the same fail-open shape #2090 exists to fix. `deviation-overdue` was green because the
parser could not see 57% of the markers; `CR finding gate` is green because it cannot see whether a
review occurred. Both report on their own blind spot and both fail in the reassuring direction — and
the second one is what let the first one ship with six defects in the fix.

The OSS skip is the **default state** for this repository, not an outage, so unless someone asks for
a review every PR here carries a `CodeRabbit` `success` that reviewed nothing and a pending
`CR findings (0 actionable)` context awaiting a review node that does not exist yet. It is not
*permanently* pending — `@coderabbitai review` resolves it — but nothing prompts anyone to ask, and
the two green-looking signals actively suggest there is nothing to ask for. The merge-gate half of
that — the pending context blocking `merge-gates.sh` through the CI bucket, where the pending count
takes no `$downgraded` subtraction and no label reaches it — was filed separately on `develop` via
PR #2094. This entry is about the review-assurance half.

## Concrete next action

1. **Distinguish "reviewed, clean" from "not reviewed".** Where the verdict line is parsed, require
   evidence naming what ran — a reviewer identity, or a tool plus run id. A self-review is legitimate
   and should stay allowed, but it should have to *say* it is one (`adversarial-code-review: self; N
   findings…`), so the next reader can price it correctly. Enumerator: every PR body on `develop`
   carrying an `adversarial-code-review:` line; today none records who reviewed.
2. **Ask for the review instead of reading the skip as a verdict.** The cheapest fix is a step that
   posts `@coderabbitai review` when the skip status appears on a PR touching a reviewed path set,
   so the opt-in is taken automatically rather than depending on someone noticing. Failing that, the
   skip should not render as `success`: it is "not reviewed", and a distinct neutral/pending
   presentation would stop it reading as assurance.
3. **Do not let a self-review satisfy the gate on a diff that changes gate logic.** A change to
   `lint-rules.d/**` or `agents/scripts/core/**` alters what every other gate can see, which is
   exactly where a blind spot is most expensive. Require a second pass for that path set.

Replaying the motivating case against (1) and (3): #2090 touches `lint-rules.d/**`, so (3) would have
required the independent pass **before** merge rather than hours after push, and (1) would have made
the PR body say `self` where it currently implies otherwise.

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-17;n=25; action=check whether any PR carrying an adversarial-code-review line records who reviewed, and whether a lint-rules.d change has shipped on a self-review alone; baseline=0 of N record a reviewer as of 2026-08-17; fired=never
