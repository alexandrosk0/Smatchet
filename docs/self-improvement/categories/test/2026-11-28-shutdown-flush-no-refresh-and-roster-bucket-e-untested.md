# Shutdown tail flush: "no grid re-read" and the roster bucket-E test are not asserted

- **Category**: test
- **Priority**: P3
- **Date**: 2026-11-28
- **Observed on**: the cache-hardening RP-3 / EM-4 follow-up PR (shutdown flush, user-roster latest-wins queue).
- **Status**: open

## What happened

`docs/plans/active/cache-hardening.md` § Verification asks for two checks the follow-up PR does not carry:

1. `AppControllerShutdown.posix` should assert that the tail flush runs a stranded `UpdateTicket` save **without** a grid re-read. The `shuttingDown_` early return in `AppController::SaveTicketThenRefresh_` is exercised by the existing case `~AppController's tail flush saves an UpdateTicket posted after the shutdown flag` (the row reaches the file and nothing crashes), but nothing counts re-reads, so a regression that re-read at quit would not fail a test.
2. EM-4's bucket-E case `UsersRoster_SavesInOrder` in `tests/ui/offline_first.test.cpp` is replaced by the bucket-A cases `~AppController leaves the newest user roster in the cache file and never an older one` and `AppController::FlushUsersRosterSaves waits for a queued roster save`, which pin the same order and flush contract without the ImGui rig.

## Concrete next action

- Give `RefreshLocalDataCheckedImpl_` (or the posix test seam) an observable re-read counter, then assert `0` re-reads across the tail flush in the posix case above.
- Add `UsersRoster_SavesInOrder` to `tests/ui/offline_first.test.cpp` only if a roster-in-the-grid behaviour is found that the bucket-A cases cannot reach; otherwise close this half as covered.
