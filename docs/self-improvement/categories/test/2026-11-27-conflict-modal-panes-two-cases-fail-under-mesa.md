# Two OfflineConflictModalPanes cases fail under Mesa in CI

- **Category**: test
- **Priority**: P2
- **Date**: 2026-11-27
- **Observed on**: the `Bucket-E Jira fixture-backend (Mesa GL, hard)` job of PR #2531 (run 46066840748), driver `scripts/dev/test-ui-offline-conflict-modal.sh`, both attempts.
- **Status**: open

## What happened

With `SMATCHET_UITEST_WITH_LOCAL_CACHE=1` the group runs for real. Locally (hardware GL, Windows) all 20 cases pass. On the Windows llvmpipe runner 18 of 20 pass and the same two fail on both attempts:

- `Text_CancelKeepsWhatWasTyped`: the `Cancel` click lands while a toast window (`##toast_hit_1`) covers it — the engine logs `Failed to move window '##toast_hit_1'! While trying to make space to click … over window 'ResolveMergeConflict'` and `Window 'ResolveMergeConflict' is not active (after aiming)`. The reopen succeeds, but the typed draft is not restored (`SmatchetOfflineConflictDraftRestoredForTest()` false; the text is the untouched conflict instead of `X` + conflict).
- `ConflictRow_ClickAndSpaceDoNotOpen`: `ItemClick '**/Conflict'` cannot hover the row — hovered id is 0 in `Smatchet - Active Project` although the row is found at the targeted position.

Both point at window state the driver does not control: the wrapped `test-ui-jira-deterministic-backend.sh` starts an unseeded profile (a fresh profile's toasts, and possibly its first-run banners, stay up; the log shows only the toast), unlike the `--seed` click-driving siblings. Whether the draft-not-restored result is only a consequence of the misdirected click, or a real Cancel-path bug when the modal is not the active window, is not established.

## Concrete next action

- Run the group under Mesa locally or in a scratch CI run with the profile seeded (or toasts dismissed before each case) and see whether both cases pass.
- If `Text_CancelKeepsWhatWasTyped` still fails with the modal not active, treat it as a product bug (Cancel through an inactive modal loses the draft) and file a GitHub Issue.
- Then add `test-ui-offline-conflict-modal` to the hard-gate loop of the `bucket-e-jira-fixture-mesa-gl` job.
