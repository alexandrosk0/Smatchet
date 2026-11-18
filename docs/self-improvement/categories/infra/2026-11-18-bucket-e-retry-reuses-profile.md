# A bucket-E retry after a deadline trip reuses the killed attempt's profile

- **Category**: infra
- **Priority**: P2
- **Date**: 2026-11-18
- **Observed on**: PR #2439 (input UX drafts and undo), bucket-E run 43947572618 and its re-runs.
- **Status**: open

## What happened

The suite outgrew the 160 s `--timeout` on llvmpipe (~163 s), so attempts 1 and 2 tripped the deadline and were killed. The retry loop then ran attempt 3 with the same user profile the killed attempts had written to (no `SMATCHET_USER_DATA` per attempt), so it booted on a non-default dock layout. It finished in time but failed `WindowExpand/SplitterDragAfterMinimizeKeepsDocking` and `WindowExpand/ScriptingWindowContributesTabBarToggle` (Log and Scripting not docked), and `ResetLayout/LiveHostProbe` logged "no canonical node realized". The failure read as a docking regression in the PR when the real cause was the deadline. The same pattern was seen earlier on #2433 and passed on a re-run.

The deadline was raised to 220 s in #2439, which removes the trigger for now but not the contamination.

## Concrete next action

In `.github/workflows/build-and-test.yml` (bucket-E run step), give each attempt a fresh profile: `export SMATCHET_USER_DATA="$RUNNER_TEMP/bucket-e-home-$attempt"` (and `SMATCHET_DB_PATH` under it) before `ui_test.run`. Also print a clear `::warning::` naming the deadline when an attempt ends in "did not finish", so a timeout is never mistaken for test failures on the next attempt.
