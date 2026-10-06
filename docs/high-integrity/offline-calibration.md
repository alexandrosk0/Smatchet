# Offline-first gate calibration (Quality Pillar 6)

This is the calibration record for the offline-first lint rules. [ADR-0026](../adr/0026-offline-first-quality-pillar.md) sets the graduation criteria:

- A WARN heuristic graduates to blocking when its whole-tree `--scan-offline` hits reach 0, fixed or deviation-marked.
- Or when fewer than 10% of its hits are false positives over about 20 PRs touching offline scope.
- Or on a maintainer decision.

`code-review` keeps this file current. Append a row to [PR tally](#pr-tally) for every PR whose `--diff` run prints an offline WARN, and re-run the whole-tree sweep when a rule's classification changes:

```bash
bash agents/scripts/project/test-lint-rules.sh --scan-offline
```

**Verdicts:**

- **TP** means true positive: the tracker being unreachable breaks what the user sees, or what they keep.
- **FP** means false positive.
- **TP-out** means the pattern is real but outside Pillar 6's tracker scope, for example Perforce. It is filed as an Issue and does not count against the rule.

## Whole-tree sweep

| Rule | Tier | S13 start | After S13 |
|---|---|---|---|
| `offline-write-bypasses-queue` | blocking, absolute-0 (S9) | 0 | 0 |
| `tracker-error-kind-collapsed` | blocking, delta per file | 3 | **0** |
| `offline-loading-only-render` | WARN | 14 | 12 |
| `offline-inflight-latch-unguarded` | WARN | 11 | 10 |
| `offline-network-read-ungated` | WARN | 5 | 4 |
| `offline-failure-cached-as-loaded` | WARN | 1 | 1 |
| `offline-cache-cleared` | WARN | 1 | **0** |

The per-hit classification below starts at S13 (35 hits at its first sweep, 27 at its last):

- **6 TP**, all fixed.
- **3 TP-out hits**, under one Issue.
- **26 FP.** Two of those left the sweep: one was rewritten to the sanctioned idiom, the other deviation-marked.

## Classification of every hit (S13, 2026-09-29)

### `tracker-error-kind-collapsed`: 3 hits, 3 TP, all fixed

| Site | Verdict | Disposition |
|---|---|---|
| `Source/Core/include/ITrackerIssueReader.h` `FetchIssuesChangedSince` default | TP | Fixed. Reads the backend's structured error; an unreachable tracker stays `Transport`. Tested in `TrackerIssueReaderDefaults.test.cpp`. |
| `Source/Core/include/ITrackerIssueReader.h` `FetchIssueKeysForView` default | TP | Fixed, same as above. |
| `Source/Core/src/Tracker/GitHubClient.cpp` `FetchIssuesChangedSince` | FP (equivalent idiom) | Rewritten as the sanctioned `structured.IsOk() ? TrackerErrorUnknown(x) : structured`. Behaviour is unchanged. |

### `offline-loading-only-render`: 12 hits after S13 (10 FP, 2 TP-out), plus 2 TP fixed in S13

| Site | Verdict | Why |
|---|---|---|
| `Tracker/TrackerGridFieldDisplay.cpp` watchers / votes window "Loading..." (two hits, gone) | TP | Fixed in S13. The windows keep the list loaded this session and draw a `DataFreshnessCue`. |
| `Tracker/TrackerGridFieldDisplay.cpp` watchers / votes Load-button tooltips | FP | The tooltip describes a disabled button while its load runs; the window itself carries the cue. |
| `TicketFieldEditor_Modal.cpp` "Loading description..." | FP | A local Markdown conversion on a worker. No network. |
| `Ui/AnnotateAnalysisUi_Window.cpp` "Loading annotated file..." | FP | A Perforce annotate, not the tracker. |
| `Ui/P4ClPreview.cpp` "Loading CL info..." (pending future, two hits) | FP | A Perforce describe in progress. |
| `Ui/P4ClPreview.cpp` "Loading CL info..." (catch arms, two hits) | TP-out | A worker exception reads as "Loading" forever. Filed as [#2270](https://github.com/alexandrosk0/Smatchet/issues/2270). |
| `Ui/SmatchetAttachmentPreviewUi.cpp` "Loading preview..." | FP | Shown only while an image's dimensions are parsed from the downloaded file. The download itself resolves offline from the S13 disk cache or fails at once with the offline message. |
| `Ui/SmatchetBulkTicketsUi.cpp` "Loading..." | FP | A local file read. |
| `Ui/SmatchetPlanDocViewerUi.cpp` "Loading..." | FP | A local file read. |
| `Plugins/LuaConsole/LuaConsolePlugin.cpp` "Loading..." | FP | A local script read. |

### `offline-inflight-latch-unguarded`: 10 hits after S13 (all FP), plus 1 TP fixed in S13

| Site | Verdict | Why |
|---|---|---|
| `Ui/SmatchetAutocompleteUi.cpp` JQL account-id resolve (gone) | TP | Fixed in S13. The resolver sends no lookup offline. An outage no longer uses up its 5-failure retry limit, which had left the query's account ids unnamed for the rest of the session. The latch is now set after the worker starts, and a worker exception is an ordinary failed lookup. |
| `AiPrefsTestConnection.cpp` | FP | An AI provider probe, not the tracker. The post-back clears the flag. |
| `Ui/SmatchetAttachmentPreviewUi.cpp` dimension parse | FP | Local image parsing. |
| `Ui/SmatchetAttachmentPreviewUi.cpp` thumbnail decode | FP | Local image decoding. |
| `Ui/SmatchetBugReportUi.cpp` | FP | The bug-report submit, not the tracker. |
| `Ui/SmatchetBulkTicketsUi.cpp` load | FP | A local file read. |
| `Ui/SmatchetBulkTicketsUi.cpp` export | FP | A local file write. |
| `Ui/SmatchetPreferencesUi.cpp` Test connection | FP | An explicit reachability probe. Offline it reports that the tracker is unreachable, which is its job. |
| `Ui/SmatchetPreferencesUi_General.cpp` update check | FP | The app-update check, not the tracker. |
| `Ui/SmatchetToolbarUi.cpp` | FP | A local views-file read. |
| `Ui/SmatchetUI.cpp` installer download | FP | The app-update download, not the tracker. |

### `offline-network-read-ungated`: 4 hits after S13 (all FP), plus 1 TP fixed in S13

| Site | Verdict | Why |
|---|---|---|
| `AppController_TicketPrefetch.cpp` (gone) | TP | Fixed in S13. The best-effort prefetch is skipped while the tracker is offline, instead of spending a worker's retry window per frame on it. The online no-backoff retry is filed as debt ([entry](../self-improvement/categories/applied.md)). |
| `AppController.cpp` `FetchIssuesForActiveView` | FP | The sync itself. `TicketSyncService` marks the tracker `TransportDown` on a transport failure and pushes the replay timers, and the grid keeps its cached tickets. |
| `AppController_CatalogAndFieldEdit.cpp` `RefreshFieldCatalog` | FP | A failed refresh keeps the catalog and shows a Warning (S1). |
| `AppController_PaneContexts.cpp` pane catalog fetch | FP | Runs after a successful sync. A failure loads the saved catalog snapshot (S12). |
| `Ui/SmatchetUI.cpp` catalog fetch worker | FP | Same failure path as `RefreshFieldCatalog`. |

### `offline-failure-cached-as-loaded`: 1 hit, 1 TP-out

| Site | Verdict | Why |
|---|---|---|
| `P4Annotate.cpp` `P4ChangelistDescribeCache::GetOrFetch` | TP-out | A failed `p4 describe` is cached for the session. Filed as [#2270](https://github.com/alexandrosk0/Smatchet/issues/2270). |

### `offline-cache-cleared`: 1 hit, 1 FP, deviation-marked

| Site | Verdict | Why |
|---|---|---|
| `Ui/SmatchetUI.cpp` backend switch `SetAvailableUsers({})` | FP (legitimate reset) | The previous tracker's users must not show under the new one, and the saved roster restores them. The rule's own text says to deviation-mark backend-switch resets; it now carries `SMATCHET_DEVIATION(rule=offline-cache-cleared; …)`. |

## Graduation proposals (S13)

**Proposed:**

- **`offline-cache-cleared`: graduate to blocking** (delta per changed file, like `tracker-error-kind-collapsed`).
  - Its whole-tree hits are 0.
  - Its one hit was the documented legitimate case, now marked.
  - The pattern is narrow: an exact `.clear()` or `SetAvailableUsers({})` on the catalog, users or components state.
- **`tracker-error-kind-collapsed`: move from delta-gated to absolute-0**, as `offline-write-bypasses-queue` did in S9. Its whole-tree hits are 0, so the move changes no current result. It removes the grandfathering path for good.

**Not proposed:**

- **`offline-failure-cached-as-loaded`** stays WARN. Its only hit is Perforce (#2270); graduate once #2270 lands and the whole tree reads 0.
- **`offline-loading-only-render`, `offline-inflight-latch-unguarded` and `offline-network-read-ungated`** are nowhere near the criteria. After S13 their remaining hits are 10 FP + 2 TP-out, 10 FP and 4 FP respectively, with no open tracker TP. The false positives share one shape: the heuristics key on *any* worker launch (`LaunchBackgroundTask`, `std::async`) and *any* "Loading" text, so local file I/O, Perforce, AI and app-update work all match. The proposed refinement, before any graduation:
  - Scope all three rules to TUs that call a tracker-facing surface: `Collaboration()`, `Reader()`, `FieldCatalog()`, `Connectivity()`, `Activity()`, or an `AppController` `Fetch*Typed` / `SearchUsersByQueryTyped` delegator.
  - Skip `SetTooltip` lines in `offline-loading-only-render`.
  - Let `offline-network-read-ungated` accept a file whose failure path is marked with a deviation naming the offline-safe fallback.

Flipping any rule is a gate change. It lands in its own PR, after the maintainer agrees here.

## PR tally

Append a row for every PR whose `--diff` run prints an offline WARN.

| Date | PR | Rule | Site | Verdict | Note |
|---|---|---|---|---|---|
| 2026-09-29 | S13 (offline-first) | all | whole-tree sweep | see above | Baseline classification: 35 hits → 27; 6 TP fixed, 3 TP-out hits (#2270), 26 FP. |
