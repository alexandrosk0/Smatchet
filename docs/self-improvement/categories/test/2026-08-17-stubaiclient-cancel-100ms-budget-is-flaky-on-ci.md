# `StubAiClient: cancel mid-stream` asserts a 100 ms wall-clock budget and flakes on CI

- **Category**: test
- **Priority**: P2
- **Date**: 2026-08-17
- **Observed on**: PR #2090, `Windows + MSVC` job 95255794038 (head `ea80edbaac09`)
- **Status**: open

## What happened

`Windows + MSVC` failed — a **required** check — on a PR whose only C++ change was comment
deletions proven token-identical to the merge base. The build was clean (776 TUs, `Compilation
failures 0`); the failure was in step 10, `Run ctest (MSVC)`, and it cascaded: every later step
(including `Build SmatchetStandalone`) was skipped, so the job also reported
`No files were found with the provided path: build/ninja-iter-msvc/Smatchet.exe`, which reads like
a link failure and is not one.

The suite ran **2904 test cases / 38380 assertions with exactly one failure**:

```
tests\Core\StubAiClientCancel.test.cpp(33):
TEST CASE:  StubAiClient: cancel mid-stream stops onDelta within 100 ms

tests\Core\StubAiClientCancel.test.cpp(98): ERROR: CHECK_FALSE( stub.CancelBudgetExceeded ) is NOT correct!
  values: CHECK_FALSE( true )
```

The assertion is a **wall-clock latency budget**, not a logic check: the cancel did land, it just
took longer than its budget on a contended runner. The file is explicitly timing-conditional around
that line — `CHECK(postCancelMs < 200)` / `CHECK(totalMs < 400)`, with a `3200` ms variant under a
slower-config guard — so the budget already needed platform-specific tuning once.

## Why it is a flake, not a regression

Three independent lines of evidence, all on the same commit:

1. **`Coverage (windows-2022 + OpenCppCoverage)` passed on the identical head** — it builds and runs
   the same doctest suite on the same OS. Same commit, same tests, one pass and one fail is the
   definition of non-deterministic.
2. The failing file, `tests/Core/StubAiClientCancel.test.cpp`, is **not in the PR diff at all**, and
   every C++ file that *is* in the diff was verified token-identical to the merge base via
   `dup_audit.normalized_stream()` — comment-only edits cannot change runtime behaviour.
3. `Mobile — POSIX core compile gate` (compiles all `CORE_SOURCES`, runs the builtin-command
   dispatch harness and the command-registry monkey) passed on the same head.

Note the run took 34.4 s for a suite that is normally much faster, and the job had a 0.13 % sccache
hit rate — i.e. this runner was doing a full cold compile of 776 TUs concurrently with the test run.
A 100 ms budget is not robust under that load.

**Confirmed 2026-08-17 02:24Z.** The next push to the same branch re-ran `Windows + MSVC` on head
`624a579`, whose C++ content is token-identical to `ea80edb`, and `Run ctest (MSVC)` passed cleanly
in 33 s. One failure and one pass on the same code is direct evidence rather than the circumstantial
argument above, and it rules out a deterministic regression. No re-run was requested to get it — the
push re-triggered the job on its own.

## Why it matters

`Windows + MSVC` is a branch-protection **required** check, so this flake blocks merges at random
and lands on whoever pushed. It also mis-presents itself: because ctest failure skips the exe build,
the surface symptom is a missing `Smatchet.exe`, which sends the reader looking for a link error.
The repo already has a quarantine mechanism for exactly this class —
`tests/Core/FlakySelfTest.test.cpp` carries a `[quarantined:test-rig]` tag — so the pattern exists
and is unused here.

## Concrete next action

Make the assertion robust rather than deleting it — the behaviour under test (cancel actually stops
`onDelta`) is worth keeping; only the *timing* half is unstable.

1. Split the assertion: keep a hard `CHECK` that `onDelta` stops after cancel (a correctness claim,
   deterministic), and downgrade the 100 ms budget to a `WARN`, or raise it to a value that reflects
   a loaded CI runner (the observed suite runtime suggests seconds, not 100 ms, of scheduling jitter
   is possible).
2. If the budget is meant as a perf guard rather than a unit assertion, move it to the perf-gate
   harness (`scripts/dev/perf-*`), which is built to tolerate machine variance, and tag the unit
   test `[quarantined:test-rig]` in the interim.

Enumerator for the sweep: `grep -rn "Budget\|_ms\b\|std::chrono" tests/Core/*.test.cpp` names every
unit test carrying a wall-clock assertion; `StubAiClientCancel.test.cpp:98` is the row that
motivated this entry, and any sibling with the same shape has the same exposure.

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-17;n=20; action=re-check whether StubAiClientCancel or another wall-clock unit assertion has failed a required check again; baseline=1 observed flake and 1 observed pass on token-identical code, 2026-08-17 (PR #2090); fired=never
