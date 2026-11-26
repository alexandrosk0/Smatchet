# ConfigSaveConcurrency "flushes on Stop" can fail under the coverage lane

- **Category**: test
- **Priority**: P3
- **Date**: 2026-11-26
- **Observed on**: PR #2496 (input UX Wave 2 leftover), `Coverage (windows-2022 + OpenCppCoverage)`, attempt 1 (job 139447158443, 2026-11-25). The PR does not touch config code. Attempt 2 failed on a Chocolatey 504 while installing OpenCppCoverage; attempt 3 passed.
- **Status**: open

## What happened

`tests/Core/ConfigSaveConcurrency.test.cpp`, case "config_save worker persists both config kinds and flushes on Stop", failed at line 118:

```
CHECK( ConfigManager::LoadAnnotateAnalysis().ChangelistCacheMaxEntries == 256 ) is NOT correct!
```

The case enqueues a tracker config and then an annotate config, calls `smatchet::config_save::Stop()`, and expects both on disk. The tracker check on the line before it passed. 4437 of the 4438 cases passed in that run. Locally the case passed 3 runs out of 3.

The likely cause, not confirmed: `Stop()` (`Source/Core/src/ConfigSaveWorker.cpp`) waits at most `kStopDrainBudget` (250 ms) for queued writes. After that it logs `config_save: drain budget exhausted; dropping pending config write(s)` and drops them by design. Under OpenCppCoverage instrumentation two config writes can take longer than 250 ms, so the annotate write is dropped and the test sees the old value. The test treats a best-effort drain as a guarantee.

## Concrete next action

- Confirm it: run the case in a loop under OpenCppCoverage, or with an artificial delay in the worker's write. Check whether the drain-budget warning is logged on the failing run. The failing job's log has no `drain budget exhausted` line, but that does not rule the cause out until it is known whether the test binary's `LOG_WARN` output reaches the job log at all.
- If confirmed, make the test independent of the budget. For example, wait until the worker has written both configs (poll the files, or add a test-only "queue empty" query) before calling `Stop()`. Or add a test-only override of the budget. Do not raise the shipping budget for the test's sake.
- If the warning is not logged, the cause is something else, such as write ordering between the two kinds: raise a GitHub Issue instead, since a lost config write is user-observable.
