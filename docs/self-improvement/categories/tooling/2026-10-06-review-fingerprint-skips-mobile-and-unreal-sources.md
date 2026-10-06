# The review fingerprint and the pre-ship clang-format pass skip `Source/Mobile/` and `Source/UnrealPlugins/`

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-10-06
- **Observed on**: the historical-review Batch 26 PR. Its `Source/Mobile/Android/android_main.cpp` change (the host `EndFrame()` call) is not in the review fingerprint, so a later edit there would not re-arm the review gate. Reported by the code-review agent's self-improvement note.
- **Status**: open

## What happened

`RA_CPP_GLOBS` in `agent-layer/agents/scripts/core/lib/review-ack.sh` decides which diff content the `.review-ack` / `.review-findings.json` fingerprint covers:

- `Source/Core`
- `Source/Plugins`
- `Source/Standalone`
- `tests`

The clang-format target globs in `scripts/dev/pre-ship.sh` have the same list; the two are kept in sync by comment. First-party C++ under `Source/Mobile/` and `Source/UnrealPlugins/` is in neither. An acked review therefore stays valid after an unreviewed edit to those trees, and pre-ship never formats them.

## Concrete next action

Add `Source/Mobile/` and `Source/UnrealPlugins/` to both lists, excluding any `ThirdParty/` beneath them, in one layer PR plus the host `pre-ship.sh` change. Better: read the list from one place, either `project.config.json` or a shared helper, so the two cannot drift again; the layer should not hard-code host paths.

Every open branch's fingerprint changes when this lands, so land it between batches and re-record acks on in-flight branches.

Bats coverage: an edit to `Source/Mobile/**.cpp` after a recorded ack re-arms the gate, and an edit under a `ThirdParty/` directory does not.
