# Plan — Grid of issues that left a view

> **Slug**: `left-view-history-grid` (matches this file's basename without `.md`).
>
> **Status**: `active`.

## Context

The user asked for "a grid to see the issues that fall out of the view when it updates, sorted by latest and with a reason". Today a departure raises one `left view` toast (`TicketChangeDiffPure.cpp` `FormatLine`) and is kept nowhere; a departure found by a full reload raises nothing.

After this lands, every issue that leaves a view is listed newest first in one dockable window, with the field change that explains it, and the list survives a restart.

The user settled the design in `AskUserQuestion`: history kept in SQLite; one dockable window across all panes and views; row actions Open in browser, Copy key / link, Dismiss (plus two-step Clear all), Open issue details; reason is field changes plus author.

## Approach

**Two capture points, one record path.** The change monitor already reports departures (`applyChangeProbeOnMainThread_`, `plan.residentRemovals`). A full reload does not, and its roster convergence cannot be used because it also drops rows a sibling pane's query admitted (`ConvergeActiveTicketsToSessionKeepSet`). The accurate signal is the view listing (`ChangeMonitorPaneState::ViewListed`, the ids the view's own query returned): old listing minus new listing, only when the query is unchanged. Listings are saved in `lookup_cache` (new kind `view_listing`), so the first reload after a launch can report what left overnight. The monitor path removes what it recorded from the listing, so a later reload cannot record it twice.

**Reason.** A worker builds it after the departure, never delaying the monitor cycle:

1. `ITrackerIssueReader::FetchIssueFieldChanges` (new, default empty; Jira overrides with one `search/jql?expand=changelog` request for the batch) gives dated, attributed from/to changes.
2. Keys with no changelog entry fall back to `FetchIssuesForKeys` and a diff of the fresh row against the last-known row.
3. `BuildLeftViewReason` (pure) prefers changes to fields the view's query names. A change to a field the query does not name is labelled as possibly related. If nothing changed it says so rather than guessing. An unreachable tracker gives "Reason unavailable".

**Storage and UI.** `LeftViewHistoryService` owns an in-memory newest-first snapshot for the UI thread; SQLite runs on workers. The window talks only to the service, so no new `AppController.h` includer is added.

The user was told the reason needs "same request count, larger responses". It keeps the existence probe (the only cross-backend 404 signal) and adds one to two batched requests per cycle in which something left. Report this in the handoff.

## Interface contracts

- **`ITrackerIssueReader`**: add `virtual Result<std::vector<TrackerFieldChange>, TrackerError> FetchIssueFieldChanges(const TrackerConfig&, const std::vector<std::string>& issueKeys)`, default `Ok({})`. `TrackerFieldChange { issueKey, fieldId, fieldLabel, fromValue, toValue, author, changedAt }` is backend-agnostic. `JiraClient` overrides it; other backends keep the default and use the snapshot diff.
- **SQLite (additive)**: new table `left_view_history(id INTEGER PRIMARY KEY AUTOINCREMENT, backend_key, pane_id, view_id, view_name, issue_key, summary, kind, reason, reason_detail_json, author, browse_url, last_known_json, left_at)`, index on `left_at`. Added to the rekey table list in `LocalCacheManager_CacheIdentity.cpp`.
- **`lookup_cache`**: new kind `view_listing`; `cache_key` = pane id + view id; payload `{ "jql": ..., "ids": [...] }`.
- **`ITicketSyncDeps`**: add `OnViewListingCompleted(const std::vector<std::string>& ids)`, called from `TicketSyncService::PublishViewListing` on a complete, non-empty reload. It replaces the direct `PublishViewListedIds` use for this purpose; the adapter forwards to `AppController`.
- **Plumbing, monitor path**: `applyChangeProbeOnMainThread_` → `recordMonitorDepartures_(ctx, paneId, prev, plan)` → `LeftViewHistoryService::Record(LeftViewCapture)` → worker → `LocalCacheManager::AppendLeftViewRows` → main thread → snapshot insert.
- **Plumbing, reload path**: `TicketSyncService::PublishViewListing` → `ITicketSyncDeps::OnViewListingCompleted` → `GridContextDepsAdapter` → `AppController::onViewListingCompleted_` → `DepartedFromListing` (pure) → same `Record`.
- **Commands**: `view.toggle.left_view_history` (alias `left_view_history`) and read-only `left_view.list` (JSON; CLI / MCP / Lua via the registry). Documented in `docs/guides/cli.md`.
- **Tunable limits**: `LeftViewHistoryMaxRows` (`left_view_history_max_rows`, 1000, 10..100000) and `LeftViewReasonMaxKeys` (`left_view_reason_max_keys`, 100, 1..1000).
- **Toast**: `SmatchetToastManager::Push` gains an overload that runs the row action on a body click, so a left-view toast opens the window.

## Files to modify

### New

1. `Source/Core/include/Sync/LeftViewReasonPure.h`, `Source/Core/src/Sync/LeftViewReasonPure.cpp`: record types, reason builder, query-field match, snapshot diff, capped last-known JSON.
2. `Source/Core/include/Sync/ViewListingDiffPure.h`: `DepartedFromListing`.
3. `Source/Core/include/LeftViewHistoryService.h`, `Source/Core/src/LeftViewHistoryService.cpp`: snapshot, worker jobs, details fetch.
4. `Source/Core/src/Persistence/LocalCacheManager_LeftViewHistory.cpp`: schema, append + trim, load, delete, clear.
5. `Source/Core/include/Ui/LeftViewHistoryPure.h`: row filter and time label.
6. `Source/Core/include/Ui/SmatchetLeftViewHistoryUi.h`, `Source/Core/src/Ui/SmatchetLeftViewHistoryUi.cpp`: window and details view.
7. `Source/Core/src/AppController_LeftViewHistory.cpp`: capture glue and service wiring.
8. `tests/Core/LeftViewReasonPure.test.cpp`, `tests/Core/ViewListingDiffPure.test.cpp`, `tests/Core/LeftViewHistoryPure.test.cpp`, `tests/Core/LeftViewHistoryStore.test.cpp`, `tests/ui/left_view_history.test.cpp`.

### Changed

- **Tracker**: `Source/Core/include/ITrackerIssueReader.h`, `Source/Core/include/Tracker/JiraClient.h`, `Source/Core/src/Tracker/JiraIssueSearch.cpp`, `Source/Core/src/Tracker/JiraChangelogDeltaPure.{h,cpp}` (all-fields sibling of `DeltasFromIssueJson`).
- **Sync**: `Source/Core/src/Sync/TicketSyncService.cpp`, `Source/Core/include/Sync/ChangeMonitorPaneState.h` (`NoteViewRowsLeft`), `ITicketSyncDeps`.
- **Persistence**: `Source/Core/include/LocalCacheManager.h`, `LocalCacheManager.cpp` (`InitSchema`), `LocalCacheManager_CacheIdentity.cpp`.
- **App**: `Source/Core/include/AppController.h`, `Source/Core/src/AppController_PaneContexts.cpp`, `Source/Core/src/GridContextDepsAdapter.cpp`.
- **Ui**: `SmatchetUiSession.h`, `SmatchetUI.cpp`, `SmatchetUI_MainMenu.cpp`, `SmatchetBottomPanelDrag.cpp`, `SmatchetToast.{h,cpp}`, `SmatchetTicketChangeNotifications.cpp`, `SmatchetLocalization.cpp`.
- **Commands / config**: `ViewToggleCommands.cpp`, a builtin command TU for `left_view.list`, `Config/TunableLimits.h`, `KeybindingsConfig.cpp` if the toggle list needs it, `docs/guides/cli.md`.
- **Build**: `tests/CMakeLists.txt`.

## Existing utilities reused

- `ClassifyMembershipRemovals` (`Sync/MembershipDiffPure.h`): which departures are resident and which verdict they carry.
- `JiraChangelogDeltaPure` internals and `BuildKeyInJql` (`JiraIssueSearch.cpp:242`): changelog parsing and key escaping.
- `FetchIssuesForKeys` (`ITrackerIssueReader.h:79`): fresh row for the snapshot diff.
- `DescribeCacheBackendKey` (`Config/CacheBackendKeyPure.h`): the Domain column; never shows the account hash.
- `SmatchetLocalization`, `SmatchetWindowExpand::BeginWindowWithToggle`, `DataFreshnessCue`: window shell and cue, as in `SmatchetNotificationCenterUi.cpp`.
- `LocalCacheManager::SaveLookup` / `LoadLookups`: saved listings.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — nothing is extracted; new TUs are new behaviour.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: the window uses `ImGuiListClipper` and a filtered index rebuilt only when the revision or filter changes; closed window costs nothing.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: every SQLite and HTTP call runs on a worker; the UI reads the snapshot only.
- **Pillar 3 (never crash)**: worker bodies catch `std::exception`; JSON goes through `json_safe::ParseBounded`; a stale post after a cache recreate is dropped by generation check.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: standard table navigation, em-based widths, theme tokens only.
- **Pillar 6 (offline-first)**: the history renders from disk with no network. Details show the saved values with a freshness cue; a failed fetch keeps them. No request while offline.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI**: fires; the grid scenarios cover the touched monitor apply path, and capture adds only a vector copy on cycles where something left.
2. **Pillar 2 static scanner**: fires; new workers carry `/* PILLAR2_WORKER_ONLY */ // est-latency:` annotations.
3. **Dispatcher drain**: N/A — only posts a short insert.
4. **Visible-cue bucket-E harness**: N/A — no new sync stall.
5. **Marker inventory**: fires if a `SMATCHET_UI_PERF_SCOPE` is added for the window draw; regenerate `docs/perf/MARKER_INVENTORY.md`.

## Risks / non-goals

- **False departures.** A changed query, a backend swap, a truncated search or an empty result must never record. Mitigation: `DepartedFromListing` requires the same query and a complete, non-empty listing; pure tests pin each case.
- **Double recording.** Monitor and reload could both report one departure. Mitigation: `NoteViewRowsLeft` and a saved-listing refresh on the monitor path.
- **Mass departure** (sprint rollover). Reason lookups stop at `LeftViewReasonMaxKeys`; remaining rows say the reason was not checked. Rows are trimmed to `LeftViewHistoryMaxRows`.
- **Comparison limits.** Non-Jira trackers can only compare fields the view fetched, so a query on an unfetched field reads "no field change found". Accepted and stated in the row text.
- **Multi-domain panes.** Monitor and fetch use the existing pane config, so the open multiple-domains bug applies here too. Not fixed here.
- **Sibling-branch overlap.** `SmatchetUI_MainMenu.cpp` is also being restructured on `claude/menu-windows-view-050a49`; this plan adds one menu item only.
- **Save formats**: additive table and lookup kind; no view or config format change beyond two new tunable keys.
- **Dual target**: no GLFW or OpenGL in the new Core headers.
- **Non-goals**: no row re-entry tracking, no change to which departures the toast reports, no MCP-specific schema beyond the registry command.

## Implementation handoff

N/A — single slice, implemented in this worktree. Order: pure + persistence → tracker → capture → UI → commands.

Pre-resolved: reads for details go through the service with no fetch while offline (Pillar 6); no tracker write is added, so the offline-queue rule does not apply; `backend_key` is the site key from the pane context.

## Verification

- **Bucket A**: `LeftViewReasonPure` (changed-field match, not-named label, no-change, unavailable, deleted, author pick, caps), `ViewListingDiffPure` (query change, empty, first listing), `LeftViewHistoryPure`, all-fields changelog parser, `LeftViewHistoryStore` against a temp SQLite file (append, newest-first load, trim, delete, clear).
- **Bucket E**: `tests/ui/left_view_history.test.cpp`: seed the service, open via the command, assert row order and reason, dismiss, two-step clear, offline details.
- **Bash-driver scenario / screenshot / sanitizer**: `Smatchet.exe cmd left_view.list --pretty` returns JSON.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs`**: run against this plan before ship; record the outcome here.
- **Manual residue**: live-tracker confirmation that a real Jira transition produces the expected reason. Bucket A covers the parser against fixture JSON; the live check is for the user, because no credentials are available to agents.

## Open questions

None — the user settled persistence, layout, row actions and reason depth.

## Out of scope (flagged, not designed)

**Deferral residue-sweep**: nothing here is deferred from an existing doc, so there are no references to revise.

- Per-pane domain correctness: the separate multiple-domains item.
- Monitor running while "Notify me" is off: the separate refresh-frequency item. Until then, with notifications off, departures are found only at reload.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
