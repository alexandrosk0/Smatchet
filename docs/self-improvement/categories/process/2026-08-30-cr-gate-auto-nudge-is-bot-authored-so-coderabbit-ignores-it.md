# `cr-finding-gate`'s auto-nudge is bot-authored, and CodeRabbit ignores bot-authored commands

- **Category**: process
- **Priority**: P2
- **Date**: 2026-08-30
- **Observed on**: PR #2176 (both heads: the initial review head and `b335714be` after the CR-Major fix)
- **Status**: open

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

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-30;n=15; action=re-check whether the never-reviewed nudge still posts as github-actions[bot] and whether any PR resolved the pending without a human comment; baseline=2 bot-nudges ignored, 2 human nudges acted on, PR #2176 2026-08-29/30; fired=never
