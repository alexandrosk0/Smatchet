# The CR rate-limit pause classifies the whole PR diff as "code", when the only thing CR has not reviewed is one markdown file

- **Category**: process
- **Priority**: P2
- **Date**: 2026-08-18
- **Found during**: un-wedging [PR #2070](https://github.com/alexandrosk0/Smatchet/pull/2070) (`branch-protection-enforce-admins`)

## Symptom

PR #2070 blocked on `cr-rate-limit-code-pr-auto-pause` and needed an operator waiver
(`cr-out-of-band` + `cr-disposition:cr-rate-limited`) to merge. The waiver attests the
operator *"consciously merged past an incomplete review"*.

The review was not meaningfully incomplete. CodeRabbit reviewed commit `a31fc8cc` in full
at 13:01:47Z with **0 actionable findings**. The head at merge time was `91a31fc6` — one
commit later, touching **one markdown file**. That single-file delta is what the quota
exhaustion prevented CR from re-reviewing, and it is what the whole waiver ceremony was for.

## Cause

[`merge-gates.sh:1356-1369`](../../../../agents/scripts/core/merge-gates.sh) picks between
two rate-limit outcomes on `pure_docs` (field 28), which is
[`is-pure-docs-diff.sh`](../../../../agents/scripts/core/is-pure-docs-diff.sh) run over the
**entire PR diff** vs the merge base:

```
if pure_docs → CR gate auto-downgraded to WARN (no label needed; markdown is never compiled)
else         → BLOCK pending CR re-review; waiver requires cr-out-of-band + cr-disposition
```

#2070's full diff contains `project.config.json`, `project.config.schema.json`,
`setup-branch-protection.sh` and `tests/bats/*.bats` — so `pure_docs=false` and it took the
CODE branch. But every one of those files had already been reviewed clean. The classifier
answers *"is this PR code?"* when the question the pause is actually asking is
*"is what CR has not seen code?"*

The stated justification for the pure-docs downgrade — *"markdown is never compiled"* —
applies exactly as well to the unreviewed delta as it does to a whole-PR docs diff. The
scope is the bug, not the policy.

## Proposed fix

1. **Classify the unreviewed delta, not the PR.** The poller already resolves the SHA of
   CR's last completed review (it must, to compute `STALE_*`). Run the same
   `is-pure-docs-diff.sh` over `<last-reviewed-sha>..<head>` and use that for the
   rate-limit branch, falling back to the whole-PR diff when no prior review exists
   (never-reviewed → the current behaviour is right). ~0.5d incl. bats cases for
   never-reviewed / docs-only-delta / code-delta.
2. **Print the delta in the BLOCK line.** `BLOCK: CodeRabbit rate-limited on a CODE PR`
   should name what is unreviewed (`3 files, 1 code` / `1 file, docs-only`) so the operator
   can size the risk without reconstructing the review history by hand from PR timeline
   comments — which is what this incident cost.

## Why it matters

Waivers are load-bearing only while they stay rare and mean something. An attestation the
operator is pushed into for a one-markdown-file delta trains the reflex of applying
`cr-out-of-band` + a disposition label without reading — and the next PR where the
unreviewed delta really is a `Source/Core` change gets the same reflexive waiver. The gate
spends its credibility on a case it did not need to block, and has none left for the case
it did.
