# The review gates never read `.review-findings.json` `outcome`, so a partial review can still open the push gate

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-11-04
- **Observed on**: the gate review of the layer's code-review prompt fix that came out of the code-review eval (pre-#18 prompt against the thinned one).
- **Status**: open

## What happened

The layer's code-review prompt now forbids recording the ack or writing the review artifact when a review ended `partial` because it skipped a Read-first criterion (the skill, the checklist, a touched leaf). Only the prompt enforces that. `record-review-verdict.sh`, `pre-ship.sh --ack-review` and `lib/review-ack.sh` in the layer check the artifact's fingerprint against the diff but never read its `outcome` field. A reviewer that skips the criteria and writes the artifact anyway opens the push gate exactly as a full review does. The eval measured that skip in 6/16 trials of the thinned prompt.

## Concrete next action

Have `record-review-verdict.sh` and `pre-ship.sh --ack-review` refuse a substantive diff whose `.review-findings.json` has an `outcome` other than `applied`, naming the outcome in the refusal. Keep a missing `outcome` accepted for one release with a WARN, so older artifacts do not wedge, then make it required. Add bats cases for `applied`, `partial`, and a missing field.
