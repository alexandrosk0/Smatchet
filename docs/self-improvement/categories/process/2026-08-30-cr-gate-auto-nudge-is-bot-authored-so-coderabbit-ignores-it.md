# `cr-finding-gate`'s auto-nudge is bot-authored, and CodeRabbit ignores bot-authored commands

- **Category**: process
- **Priority**: P2
- **Date**: 2026-08-30
- **Observed on**: PR #2176 (both heads: the initial review head and `b335714be` after the CR-Major fix)
- **Status**: applied — 2026-09-09 (OSS manual-trigger playbook: labeled trigger + terminal failure + merge-gates discount + scripts/dev/trigger-coderabbit-review.sh)

## What happened

On a sub-10-star repo CodeRabbit posts `Review skipped: manual review required for this OSS
repository` on every head. `cr-finding-gate`'s `maybe_nudge_review never-reviewed` rung
(`action.yml:511`) is supposed to break that state by commenting `@coderabbitai review` — but the
comment posts via the workflow's `GITHUB_TOKEN`, i.e. authored by `github-actions[bot]`, and
**CodeRabbit does not act on bot-authored commands**. The nudge lands, CR stays silent, the
context stays `pending "awaiting CodeRabbit review on current head"` forever.

Observed twice on #2176: after each push the gate's own nudge produced nothing; the pending
resolved ONLY once a **human-authored** `@coderabbitai review` PR comment was posted out-of-band
(sanctioned human-nudge carve-out at `merge-gates.sh:149-150`). After the human comment CR
reviewed the head, the context went terminal, and the merge proceeded on real review evidence —
so `merge-gates.sh`'s block-on-any-red held correctly; the dead rung just converts "auto-heal"
into "silent wedge until a human notices".

## Relationship to existing entries

[`2026-08-19-cr-finding-gate-posts-an-unbounded-pending-no-layer-blocks-on.md`](2026-08-19-cr-finding-gate-posts-an-unbounded-pending-no-layer-blocks-on.md)
documents that the never-reviewed refusal has no terminal arm (fails silent). This entry adds the
*reason the wait can never self-heal*: the one automated actor that could end it speaks with a
voice CodeRabbit is deaf to. Fix both together — a terminal arm makes the wedge visible; a
human-credentialed nudge (or dropping the dead rung) makes the auto-heal real.

## Concrete next action

1. Make the nudge human-credentialed: post it with the orchestrator's user token (`ORCH_USER`
   path already exists in `safe-merge.sh`) from the *caller* side rather than the workflow side —
   or delete the `never-reviewed` nudge rung outright and document that resolution requires a
   human comment, so the gate does not pretend to an ability it lacks.
   Enumerator: `grep -n 'maybe_nudge_review\|GITHUB_TOKEN' .github/actions/cr-finding-gate/action.yml`.
2. Assert in `tests/bats/cr_finding_gate.bats` that whichever path remains is honest: either the
   nudge is posted with a non-bot credential, or the never-reviewed branch posts a terminal
   verdict naming the human action required.

## Follow-up observation — 2026-09-12 (PR #2184)

Re-checked on PR #2184. The finding **reproduced, and the dead rung is still in place**, but with one
correction to how "applied" should be read.

- **The rung still posts as `github-actions[bot]`.** `.github/actions/cr-finding-gate/action.yml:158-165`
  documents it in its own comment: *"the nudge posts via the workflow token, i.e. as
  `github-actions[bot]`"*. The 2026-09-09 remedy shipped
  `scripts/dev/trigger-coderabbit-review.sh` — a **human-run playbook**, which is a real improvement
  to the recovery path but does not repair the automated rung. Action 1 of this entry ("make the
  nudge human-credentialed, or delete the rung outright") is therefore still open, and the entry's
  `Status: applied` overstates coverage.
- **Bot nudge 0 for 3 on one PR.** #2184 fired auto-nudges at 2026-09-06T19:03:11Z,
  2026-09-07T00:02:27Z and 2026-09-07T18:59:32Z. The first two were ignored for ~20 h and ~15 h; the
  third was likewise never acted on. All three pre-date the 2026-09-09 remedy, so they are evidence
  about the rung, not about the playbook.
- **Human nudge acted on in 7 seconds, twice.** A human-authored `@coderabbitai review` at
  2026-09-07T15:37:15Z drew a CodeRabbit reply at 15:37:22Z (7 s). A second at 20:08:15Z drew a reply
  at 20:08:20Z (5 s) and the completed review that produced 2 actionable findings. The asymmetry is
  now 0/3 bot vs 2/2 human on this PR alone, or **0/5 bot vs 4/4 human** counting #2176.
- **New adjacent fact worth folding in:** the first human nudge was refused with *"Review rate
  limited"*, and CodeRabbit's summary comment states the real constraint — *"Your plan provides up to
  1 included review per hour; 0 remain after this review"*. So on this repo the recovery path is
  bounded by an OSS quota as well as by authorship; a human-credentialed nudge fixes the authorship
  half only, and the terminal-arm work in
  [`2026-08-19-cr-finding-gate-posts-an-unbounded-pending-no-layer-blocks-on.md`](2026-08-19-cr-finding-gate-posts-an-unbounded-pending-no-layer-blocks-on.md)
  is what makes the quota wait *visible* rather than silent.

Suggested status correction: `applied` → `partially applied` (human playbook shipped; automated rung
unchanged).

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-30;n=15; action=re-check whether the never-reviewed nudge still posts as github-actions[bot] and whether any PR resolved the pending without a human comment; baseline=2 bot-nudges ignored, 2 human nudges acted on, PR #2176 2026-08-29/30; fired=2026-09-12
