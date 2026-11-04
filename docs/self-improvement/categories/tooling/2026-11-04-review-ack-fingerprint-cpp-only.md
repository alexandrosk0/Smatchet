# The review ack's fingerprint covers only the first-party C++ diff, so a post-review edit to a gate script, workflow, doc or the layer pin leaves the ack fresh

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-11-04
- **Observed on**: Smatchet#2389. After the code-review gate's High finding, `scripts/dev/check-layer-pin.sh` changed, and the recorded fingerprint (`a3377c35…`) did not move.
- **Status**: open

## What happened

`ra_fingerprint` in `agent-layer/agents/scripts/core/lib/review-ack.sh` hashes `git diff -- "${RA_CPP_GLOBS[@]}"` only (`Source/Core`, `Source/Plugins`, `Source/Standalone`, `tests`), in both staged and branch mode. `.review-findings.json` and `.review-ack` are pinned to that hash, so an edit made after the review to any other file still matches the ack:

- a gate or hook script;
- a CI workflow;
- `project.config.json`;
- the `agent-layer` gitlink, which swaps every gate script at once;
- docs.

`record-review-verdict.sh` and `pre-ship.sh --ack-review` then accept the stale review. Here the edit was the reviewed fix, so no harm was done, but nothing distinguished it from an unreviewed change.

## Concrete next action

In the layer, add a second component to the fingerprint: a hash of the non-C++ diff over the review-enforcement surface (`RA_ENFORCEMENT_GLOBS`, which already lists those paths), or of the whole diff. `.review-findings.json` and the ack then go stale on any post-review edit there. Keep the C++-only "substantive" test as it is, since it decides whether proof is required at all. Add bats cases for both: an edit to `scripts/dev/*.sh` after the ack must refuse the verdict, and an edit to a doc outside the surface must not, if the whole-diff variant is not chosen. Until then, document in `agents/core/code-review.md` § Gate artifacts that only the C++ diff is pinned.
