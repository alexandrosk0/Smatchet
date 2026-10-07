# A runner-image toolset change reds a push-only lane with no introducing commit, and the develop-tip nudge is the only thing that reports it

- **Category**: infra
- **Priority**: P2
- **Date**: 2026-10-05
- **Observed on**: develop runs 36791648339 (`cb98ca0e7`, 2026-09-30) through 37269643464 (`7abcd7195`, 2026-10-05) — `Windows-on-ARM native build + ctest (atomics gate, runner-gated)` red on all 12 runs that executed it. Product bug: Issue #2303 (sibling: #2304).
- **Status**: open

## What happened

Between the lane's last green run (36641344648, `80b85feb1`, 2026-09-29) and its first red one, the `windows-11-arm` hosted image moved from Visual Studio 2022 / MSVC 14.44.35207 to Visual Studio 18 / MSVC 14.51.36231. The 14.51 STL enforces a much smaller `std::regex` complexity budget (microsoft/STL#6005), and `ParseCallstackText` let the resulting `std::regex_error` escape, failing one doctest case. The only commit in that window, #2272, is a deviation-marker chore that touches none of the code involved.

The lane runs on `push` and `workflow_dispatch` only and is not a required context, so no PR check went red and no merge gate read it. `agents/scripts/core/develop-tip-required-green.sh` did report it at SessionStart, as designed. Its block says to root-cause "the introducing commit/PR" and names the tip's PR, which here was whichever unrelated PR had merged last. No Issue existed for the red until 2026-10-05.

While the lane was red, the two steps after ctest — `DX12-on-WARP launch smoke (ARM64)` and `Perf PR-fast bootstrap (ci-windows-arm64, DX12/WARP)` — were skipped on every run, so that coverage was absent for the five days as well.

## Why it matters

This is the third recorded time a red check on the develop tip has stood across later merges (`postmortems.md` 2026-07-10 · #1698 and 2026-08-05 · #1957). The gate those entries produced is the SessionStart nudge. It fired here and the red still stood for 12 runs, for two reasons the nudge cannot address as written:

- It prints the same block to every session and keeps no record, so a session cannot tell a red that appeared an hour ago from one that has stood for days, or whether another session already has it.
- It attributes the red to a commit. A hosted-image change has no commit, so the attribution points at an innocent PR.

The x64 lanes pin `windows-2022`, so the first sign of a new MSVC STL's behaviour arrives on this one non-required, push-only lane. #2304 (the logger's redaction pass throws under the same limit) was found only by reading the STL change, not by any lane.

## Concrete next action

1. **Give a standing develop red an age and a tracked record.** In `develop-tip-required-green.sh`, for each not-green check on the tip, walk that workflow's develop `push` runs newest-first (`gh api repos/<owner>/<repo>/actions/workflows/<id>/runs?branch=develop&event=push`, then each run's `jobs` filtered by the check name), skipping runs where the job did not execute, and count consecutive failures back to the last success. Print `red since <sha> (<date>), <N> runs`. When `N >= 3`, create or refresh one GitHub Issue labelled `build-break` titled with the check name, the way the stale-lock sweep maintains its Issue, and print its number in the nudge.
   Replay against this incident: the enumerator is the run list above for workflow `build-and-test.yml`; for the check `Windows-on-ARM native build + ctest (atomics gate, runner-gated)` it yields failure on 36791648339, 36821792167 and 37153470821 in that order, with success on 36641344648 before them. The third consecutive failure is 37153470821 (its job finished 2026-10-03T21:11Z), so the Issue would have existed about 39 hours before #2303 was filed by hand (2026-10-05T12:44Z).
2. **Say when the first red commit cannot be the cause.** In the same walk, if the first-red run's commit changed no file under `Source/`, `tests/`, `CMakeLists.txt`, `cmake/` or `.github/workflows/` relative to the last-green run's commit (`git diff --name-only <lastGreen> <firstRed>`), print `no build input changed between the last green and the first red run — suspect the runner image` instead of naming a PR. Replayed here: `git diff --name-only 80b85feb1 cb98ca0e7` lists `Source/` and `tests/` files (the deviation-marker comments), so this heuristic alone would **not** have fired; it needs the toolset line from item 3 to be decisive.
3. **Record the toolset in the job summary.** Add one step to the Windows jobs in `.github/workflows/build-and-test.yml` that writes `VCToolsVersion` and the `cl` version to `$GITHUB_STEP_SUMMARY`. The nudge walk in item 1 can then compare the last-green and first-red values and print `toolset changed 14.44.35207 -> 14.51.36231`. Replayed here: that line appears on run 36791648339 and names the cause directly.
4. **Make the branch run of a push-only lane a ship-loop step.** When a fix targets a lane that skips `pull_request`, no PR check can show it working. `.github/workflows/build-and-test.yml` documents the `workflow_dispatch` route in a comment, but no rule in `docs/agent-rules/ship-loops.md` requires it, and the dispatch builds only when the branch's tip commit changes code (the `changes` job diffs `HEAD~1...HEAD`). Add a pre-merge item to § [pre-first-push gate]: dispatch the lane on the branch, cite the run in the PR's test plan, and re-dispatch after any push. Replayed here: the fix for #2303 passes every PR-time lane with or without its `catch`, so the dispatched Windows-on-ARM run is the only pre-merge evidence that it works.
5. **Maintainer decision, not an agent one:** whether a PR-time x64 lane on the newest hosted Visual Studio image is worth its minutes. It would have put both #2303 and #2304's class on a PR-visible check when the image moved.
