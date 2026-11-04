# The review gates never read `.review-findings.json` `outcome`, so a partial review can still open the push gate

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-11-04
- **Observed on**: the gate review of the layer's code-review prompt fix that came out of the code-review eval (pre-#18 prompt against the thinned one).
- **Status**: open

## What happened

The layer's code-review prompt now forbids recording the ack or writing the review artifact when a review ended `partial` because it skipped a Read-first criterion (the skill, the checklist, a touched leaf). Only the prompt enforces that. The layer's `record-review-verdict.sh` and the host's `scripts/dev/pre-ship.sh --ack-review` read the artifact through one shared parser, `ra_findings_fingerprint` in `agent-layer/agents/scripts/core/lib/review-ack.sh`; it checks the fingerprint against the diff and never reads anything else. A reviewer that skips the criteria and writes the artifact anyway opens the push gate exactly as a full review does. The eval measured that skip in 6/16 trials of the thinned prompt.

## Concrete next action

A two-repo change. In the layer: have the code-review prompt write a `criteria_skipped` list (empty when every Read-first criterion was applied) into the artifact, add an `ra_findings_criteria_skipped` reader beside `ra_findings_fingerprint` in `lib/review-ack.sh`, and have `record-review-verdict.sh` refuse a substantive diff whose list is non-empty, naming the skipped criteria. Key on that field, not on `outcome`: a review can end `partial` for other reasons (a missing analyzer) and still be a valid gate review. Accept a missing field for one release with a WARN so older artifacts do not wedge. Then, in the host, pin the layer and call the same reader from `scripts/dev/pre-ship.sh --ack-review`. Add bats cases for an empty list, a non-empty list and a missing field.
