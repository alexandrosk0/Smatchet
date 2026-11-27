# OfflineConflictModalPanes reports a pass with every case skipped

- **Category**: test
- **Priority**: P2
- **Date**: 2026-11-27
- **Observed on**: a Linux bucket-E run for the dialog-contract Esc-sites follow-up (`ui_test.run --name=OfflineConflictModalPanes --spawn`).
- **Status**: open

## What happened

`tests/ui/offline_conflict_modal_panes.test.cpp` (`EnterConflictHost`) logs `SKIP: SMATCHET_UITEST_WITH_LOCAL_CACHE not set …` and returns when the local-cache opt-in is missing. The test engine counts a case that returns early as passed, so a run without the variable reports `passed 19 / failed 0` while no case touched the resolver. A first baseline and a mutation run both read as green or as "survived" for that reason; the group only executed once `SMATCHET_UITEST_WITH_LOCAL_CACHE=1` was exported.

Nothing in the result says a case skipped: the count of skipped cases is only in the per-test log, which the spawn driver does not print.

## Concrete next action

- Make the skip visible in the result: either fail the case when a required env variable is missing and the run is not under the software-GL carve-out, or have the driver (`scripts/dev/test-ui-*.sh`, `scripts/dev/lib/ui-test-driver.sh`) count `SKIP:` lines in the spawn log and print `skipped N` next to the pass count.
- Export `SMATCHET_UITEST_WITH_LOCAL_CACHE=1` in whichever driver runs this group, so a local run covers the resolver by default.
- Check the other groups that begin with a `SKIP:` guard (`data_dependent_windows_smoke.test.cpp` uses the same variable) for the same fail-open shape.
