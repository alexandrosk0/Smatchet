# Plan — Delta reload and cache-everything for tickets

> **Slug**: `delta-sync` (matches this file's basename without `.md`).
>
> **Status**: `active` — implemented locally as seven stacked branches (`feat/delta-sync-p1-races` … `-p7-lookups`); Phase 0 (live-Jira spike) is an open item.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan-doc family. Routed to: tracker-backend, offline-sync, command-system, grid-engine.

## Context

Every reload re-downloads the whole view: all `/search/jql` pages with full bodies, `description`, `comment`, and an unconditional `expand=changelog`. It then re-asks for every parent and child (up to 16 hops each) and rewrites every SQLite row even when nothing changed. The cache (`tickets_v2`) stores no `updated`/version data, so there is nothing to diff against.

Known races sit next to it:

- a change-monitor probe can overwrite newer synced rows;
- the `sync.*` commands run off the UI thread when called from MCP;
- the parent and child walks cannot be cancelled.

Lookups are only partly cached: the field-catalog snapshot is ignored at startup, `lookup_cache` has no TTL, and transitions go stale when a ticket changes remotely.

**Intended outcome — after this lands:** a ticket that was loaded and has not changed is never downloaded again. A reload costs one lightweight roster request (keys + `updated`) plus full downloads only for changed tickets. Any error on the new path falls back to today's full fetch. Lookups respect a TTL.

## Approach

Seven phases, one branch each, stacked. `SyncMode { Delta, Full }` selects the path; Refresh View and `sync.incremental` run Delta, `sync.full` and Shift+Refresh force Full. A separate `ticket_sync_meta` table (not a column on `tickets_v2`, which is written with `INSERT OR REPLACE`) records `remote_updated`, the field list the row was fetched with, and `synced_at`. An empty meta table means "unknown", so the ticket is refetched and no migration flag is needed.

Invariants taken from the scoped rules: `SupportsVersionedRoster()` is a capability flag (default false); roster calls return a typed `TrackerError` so `IsRetryable()` keeps working; HTTP goes through `TrackerGetLogged`; no Jira types in shared interfaces; `cache.recreate` is the only destructive wipe; no SQLite on the UI thread (Phase 4); single-slot async loads use `LaunchIntoSlot` / `TakeFromSlot`.

### Phases

| # | Branch | Scope |
|---|---|---|
| 1 | `feat/delta-sync-p1-races` | `sync.*` commands hop to the UI thread; change-probe epoch guard + per-row "never replace newer with older" (`Sync/RemoteVersionPure.h`); cancellable parent/child walks; `SyncSessionStats` + read-only `sync.stats` |
| 2 | `feat/delta-sync-p2-meta` | `ticket_sync_meta` table; `ISyncCache` meta methods (non-pure defaults); always request/map `updated`; `expand=changelog` only when `history` is selected; one `BuildJiraSearchUrl`; `Sync/DeltaSyncPure.h` |
| 3 | `feat/delta-sync-p3-delta` | `FetchViewRoster`; `RunDeltaFetch` / `RunFullFetch`; pure `ChooseSyncStrategy`, `ComputeDeltaFetchSet`, `ShouldPreferFullFetch`; `CachedUnchanged` batches; `sync.full` stops wiping (new destructive `cache.recreate`); config `DeltaSyncEnabled`, `DeltaSyncMaxRowAgeSec` |
| 4 | `feat/delta-sync-p4-offui` | SQLite writes leave the UI thread: worker saves rows + meta before queueing; stale deletion is one worker-side transaction |
| 5 | `feat/delta-sync-p5-hierarchy` | delta for the parent/child walks (`FetchRosterForKeys`, `FetchChildRosterOfKeys`) |
| 6 | `feat/delta-sync-p6-monitor` | change monitor uses one roster call (`DiffRosterAgainstResident`), or the legacy changed-since path when the roster is cut short (the pane remembers that until its next reload); new keys join the grid |
| 7 | `feat/delta-sync-p7-lookups` | field-catalog snapshot TTL; transitions/editmeta invalidation on remote change; `lookup_cache` TTL; comments modal skips refetch for an unchanged ticket |

### Fallback matrix

| Condition | Action |
|---|---|
| Forced, delta disabled, backend has no roster support, or no meta yet | Full |
| Roster error | Full |
| Roster incomplete (cap hit or warning) | Delta for the rows seen; no stale purge |
| More than 50% changed and more than 100 rows | Full |
| A delta body request for a roster-listed key is refused (400/404/408/410/413/414/422) | Never authoritative: the view falls back to Full and records the reason; in a hierarchy hop the walk becomes non-authoritative (no purge) and names the keys |
| Other body error | Full |
| Cached row missing | Fetch it |
| Hierarchy roster error | Today's full-body hop |
| Cancelled | Stop; no fallback |
| Empty roster but non-empty cache | Existing empty-sync guard |
| Probe row older than the resident row, or the sync epoch moved | Dropped |

## Files to modify

Grouped by phase; the plan's full file list is the git diff of each branch.

- **P1**: `Commands/Builtin/BuiltinCommands_Sync.cpp`, `BuiltinCommands.cpp`, `BuiltinCommands_Internal.h`, `Interfaces/IAppSync.h`, `Sync/TicketSyncService.{h,cpp}`, `AppController_PaneContexts.cpp`, `PaneSyncKickPolicy.h`, `ITrackerIssueReader.h`, `Tracker/JiraIssueSearch.cpp`, new `Sync/RemoteVersionPure.h`.
- **P2**: `Persistence/LocalCacheManager.cpp`, `Persistence/ISyncCache.h`, `Tracker/JiraIssueMappingPure.cpp`, `Tracker/JiraIssueSearch.cpp`, new `Sync/DeltaSyncPure.h`, `tests/support/FakeSyncCache.h`.
- **P3**: `ITrackerIssueReader.h`, `Tracker/SearchFetchGuards.h`, `Sync/TicketSyncService.cpp`, `Interfaces/IAppSync.h`, `Ui/SmatchetGridHeaderUi.cpp`, `Ui/SmatchetGridUiSupport.cpp`, config files, `docs/guides/cli.md`.
- **P4–P7**: see the phase table; each follows the matching section of the approved plan.

## Existing utilities reused

- `FetchKeysIsolatingRefusals` (`Sync/TicketSyncService.cpp`) — refusal isolation for the changed-body fetch.
- `JiraRunSearchPageLoop` (`Tracker/JiraIssueSearch.cpp`) — roster paging.
- `RunOnUiThreadAsCommandResult` (`Commands/MainThreadDispatch.h`) — command thread hop.
- `ShouldSkipMassDeletionOnEmptyFullSync` — empty-roster guard.
- `ParentHierarchyPure::ParentKeyOf` — the one parent-field contract.
- `NetworkUsageTracker` — request/byte deltas for the stats record.

## Extraction sizing

N/A — nothing is extracted from an over-cap file; new logic lands in new pure headers.

## UX Pillar callouts

- **Pillar 1 (perf)**: Delta sync lowers per-reload work; the apply path gains an id→index map instead of a linear `find_if`, and skips republishing when nothing changed.
- **Pillar 2 (UI never blocks)**: Phase 4 moves the remaining UI-thread SQLite writes onto the worker; Phase 1 puts `sync.*` on the UI thread only for the cheap state change.
- **Pillar 3 (never crash)**: every new path is wrapped by the existing worker try/catch; failures fall back to Full.
- **Pillar 4 (accessibility)**: no impact; no new widgets.

## Perf-review-system gates

1. **PR-fast CI** — scenario most directly exercising the path: the sync/concurrent-sync scenario (`agents/core/perf-gatekeeper.md` map).
2. **Pillar 2 static scanner** — Phase 4 removes UI-thread SQLite writes; no new sync I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue harness** — no new > 100 ms stall path.
5. **Marker inventory** — no new `SMATCHET_UI_PERF_SCOPE` markers.

## Risks / non-goals

- Data copied from other issues (parent summary, link status, sprint, display names) does not move `updated`: mitigated by `DeltaSyncMaxRowAgeSec` (default 6 h) forcing a refetch of old rows.
- Comment edits/deletes may not move `updated` on every Jira instance: Phase 0 must confirm; until then the max-row-age bound limits staleness.
- Roster page size and `kJiraMaxRosterPages` use conservative defaults (`maxResults=100`) until Phase 0 measures real limits.
- Non-goal: Plane/GitHub roster support (they keep the old path via `SupportsVersionedRoster() == false`).

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `ctest --preset ninja-test-msvc -R "DeltaSync|RemoteVersion|TicketSyncService|SyncCacheContract|LocalCacheTicketSyncMeta|JiraIssueSearchHttp|JiraIssueMappingPure|SyncEmptyFullSyncGuard|ConfigManager|KeyedLookupCache|FieldCatalogCache"` plus the full suite.
- **Request counts**: `JiraCatalogHttpFixture` counter — a second delta reload is 1 roster page and 0 body requests; Full equals today's count.
- **Live check** (needs a real Jira — Phase 0): `Smatchet.exe cmd sync.full --yes`, then `sync.incremental` twice; `sync.stats` shows `mode=delta`, `bodiesFetched=0`; edit one issue, rerun, `bodiesFetched=1`.
- **Races**: `sync.incremental` over MCP under the sanitizer build; `concurrent-sync` scenario flat or better.
- **Data safety**: after `sync.full`, the offline queue survives.
- **Build gate**: `cmake --build --preset ninja-test-msvc`; dual-target `SmatchetStandalone SmatchetCore_DX12` where a preset is available.
- **Doc validation**: `scripts/dev/test-docs.sh` (not runnable when the `agent-layer` submodule is not checked out).
- **Plan stress-test — `grill-with-docs`**: not run in this implementation pass; flagged for the reviewer.

## Out of scope

- **Phase 0 (open item)** — a spike against a live Jira instance: measure the page size for `fields=updated&maxResults=5000` versus `fields=key`; whether `updated` changes on comment edit and delete; read-after-write consistency. Its results set the roster page cap and the max row age. Until then: roster `maxResults=100`, named constant `kJiraMaxRosterPages`, `DeltaSyncMaxRowAgeSec` 21600.
- Plane / GitHub / Linear roster implementations — follow-up per backend.

## Review follow-up decisions

The Phase 3-7 review follow-ups that needed a product or design decision are decided below (nothing is open). Everything else the reviews found is fixed on `feat/delta-sync-followups` (the change monitor's per-pane memory, stamp comparison by instant, view-field trimming, the roster-scoped delta precondition, the per-session apply map, rows the cache holds newer, remembered refused keys (bounded at 256, expired after `DeltaSyncMaxRowAgeSec`, forgotten once a fetch serves the key's body at the version it was refused for or later: `RefusedKeySet`), the local-refresh ordering gate, cache-served change reports, `config.set` type checks, the one-parse catalog restore, the Fields tab Refresh, `cache.recreate` wipe reporting, the per-file LRU touches). The change-monitor probe-save race was checked and is obsolete: `SaveTicketsWithSyncMeta` refuses a row whose incoming `updated` is provably older than the stored one (tests in `LocalCacheTicketSyncMeta.test.cpp`), so a probe save that lands after a sync started cannot roll a stored row back; a Full sync bypasses the guard on purpose.

- **Local edits: pending overlay (decided and done, option C).** The row keeps its sync meta, so a delta reload still serves an unconfirmed local edit as unchanged; the grid now says so. `BuildPendingOverlay` (`Sync/PendingOverlayPure.h`) indexes the in-memory offline-queue snapshots (`pending_field_edits` and `pending_actions`, queued and dead-lettered; no SQLite) by tracker namespace, issue and field, and `RefreshPendingOverlay` rebuilds it only when a snapshot is replaced (called at the start of `drawActiveProjectGridRows` in `SmatchetActiveProjectGridTable.cpp`, with the pane's `PaneCacheBackendKey`; the row loop looks each visible row up with `PendingRowCueFor`, the key cell draws it, and a value cell takes its badge from `PendingCellCueFor`; the lookups are covered by `PendingOverlayPure.test.cpp`). A row with any such edit or action shows "pending" (or red "not sent" when one is dead-lettered, ambiguous, held for review or in a merge conflict) after its key, with a tooltip that points at the offline queue panel above the grid; the cell of a queued field edit gets a `~` (or red `!`) badge through the existing cell write-state badge. Pending creates are not rows of the grid and are not part of it.
- **Off the UI thread (decided and done).**
  - *Catalog save.* `SetFieldCatalog` / `RefreshFieldCatalog` queue the snapshot write on a `SerialTaskQueue` (`AppController::catalogSnapshotIo_`), in call order, on copies of the apply's own vectors. The queue drains as a background task, so `JoinBackgroundTasks` waits for it, and `~AppController` flushes it (bounded, 5 s) before the pool is flagged. `FlushFieldCatalogSnapshotIo` lets the Linux-only `AppControllerFieldCatalog.posix.test.cpp` wait for a save before it reads the snapshot back.
  - *Catalog restore.* The startup restore (views file, project resolve, snapshot parse) runs on a `std::async` worker in a single slot (`LaunchIntoSlot` / `TakeFromSlot`); `PollStartupCatalogRestore` installs it once per frame. The grid's fetch-or-skip decision for the startup catalog waits for it (`StartupCatalogDecisionMustWait`), so the skip still reads the restored snapshot's age, project scope and columns. A restore never overrides a catalog that landed first or lands on a pane that moved to another tracker (`DecideStartupRestore`, tested exhaustively). The render-path error handler reads the snapshot on the snapshot queue's worker (after any queued save) and applies the restore and the banner from a completion post; it latches the pane's backend generation and cache key at launch (`CatalogWriteGuard::LatchedTo`, rule `PaneLatchHolds`) and drops the result, restore and banner, if either moved by the time it lands. The Fields tab shows "Loading saved fields..." (`IsFieldCatalogRestoring`, per pane: a restore for one pane does not make another read as loading), and a pane with no rows reads as loading while a restore is pending. A second `Initialize` waits for the first startup read on the UI thread (one small file read, never per frame); that wait is deliberate and commented.
  - *`cache.recreate`.* `AppController::StartRecreateLocalCacheDatabase` runs the teardown on a worker behind a "recreating" gate. On the UI thread it flags every pane's sync cancelled, holds back new syncs (`TicketSyncService::BeginCacheTeardown`: `SyncWithBackend` records the request - latest config and views, Full sticky - and `EndCacheTeardown` replays it, so a pane opened mid-recreate or a views load that finished meanwhile still syncs; `TickStreamingApply` idles) and hands each sync's worker thread to the recreate worker, which joins them. It then asks the UI thread to withdraw the cache (`atomic_exchange`, between frames, so no UI code holds a raw pointer into it), joins the background tasks, drops the last reference (SQLite closes on the worker), deletes the files and opens and sets up the new database. A completion post on the UI thread resets the services (`EndCacheTeardown`, which replays the held-back request), refreshes the queue views and every live pane (the focused one directly; the others are re-read from the new database and their first-sync latch is re-armed, `RefreshOtherPanesAfterCacheWipe_`), lowers the gate, pushes the result toast and calls `onDone` (the follow-up full sync). A second recreate while one runs is refused. *What the gate does not cover:* the offline queues live in the database and are wiped with it by design; an enqueue (field edit, action, create) in the withdraw window finds no cache and fails at once with a "being recreated, retry in a moment" message (`IOfflineQueueDeps::IsCacheRecreating`), never silently queued into the file about to be deleted. *No frame loop:* the one-shot in-process CLI (`Smatchet.exe cmd cache.recreate --yes` without MCP) runs the command on the UI thread but never draws a frame, so `IMainThreadPoster::HasFrameLoop()` is false there (`CliCommandRunner` lowers it around the dispatch) and the command pumps `DrainMainThreadOnce()`, bounded by `--timeout`, until the outcome arrives; an unfinished recreate at the bound is a failure. Verified by driving the real worker path on a file database (`AppControllerCacheRecreate.posix.test.cpp`, Linux target; also built and run on Windows through a hand-linked executable while preparing this change). *Reporting:* a CLI, MCP or Lua caller waits off the UI thread and gets the final result (`{triggered, completed: true, wiped}`, or a failure that says whether the cache was wiped); the command palette (UI thread) gets `{started: true, completed: false}` at once and the toast reports the outcome. The Preferences button uses the same call and is disabled, with a cue, while it runs. Documented in `docs/guides/cli.md`.
- **Anything else asynchronous: the delta-sync sweep (done).** Scope: sync, cache, catalog, lookups, the change monitor and the comments modal (`pillar2-scan.sh` is clean on every UI file; the rest was read by hand).
  - *Moved.* (1) `TicketSyncService::SyncWithBackend` with no views override (the `sync.*` commands, the cache-recreate follow-up) read and parsed `smatchet_views.json` on the UI thread: it now loads them on a worker and starts the sync from a UI-thread post, guarded by an `alive_` token against a retired pane (`ITicketSyncDeps::PostToMainThread`), and counted in `TicketSyncService::HasPendingWork()` so the pane is not retired (its context owns the deps the worker posts through) while the read runs. (2) The startup `RefreshLocalData()` (a full-table SQLite read in `InitFieldCatalog`) is `RefreshLocalDataAsync()`. (3) `ListCachedProjects()` on the open edge of the Preferences Tracker tab and the JQL project pill, and the Preferences "Forget" button's `ForgetProject()` file write, run on a worker in a single slot (`RefreshCachedProjectsSnapshotOnOpen` / `RequestForgetCachedProject`); a read that lands after a Forget drops the forgotten entry (`RemoveForgottenProjects`), and at exit the read is joined and the still-queued Forgets are applied (`DrainUiDrawSessionFuturesBeforeAppTeardown`). (4) Race hardening that the worker-side cache swap needs: every plain `Cache` read that was checked and then dereferenced is now one `atomic_load` latch (the AI chat load, the offline-create worker, the probe-row persist and delete, the pane seed read, the ticket prefetch, `GridContextDepsAdapter::Cache()` / `HasCache()`).
  - *Already off-thread, checked.* The change monitor (probe fetch, `persistProbeRows_`, `dropUnheldCacheRows_` deletes), the comments modal (saved thread parsed on the load worker, seed from the in-memory snapshot), the lookups (`LoadProjectList`, the saved components read run inside `RunKeyedFetch` workers), the pane seed read in `EnsurePaneLiveSyncStarted`.
  - *Left on purpose.* (a) The streaming apply and the stale-row deletion in `TickStreamingApply` write to SQLite on the UI thread in 3 ms / 20-ticket frame budgets: that is the documented apply design (the in-memory merge and the cache write must stay ordered with the grid snapshot), and moving it is a redesign of the apply FSM, not a sweep item. (b) `TicketSyncService` joins a *finished* sync worker on the UI thread (`StartPendingSyncIfIdle`, `StartStreamingSync`); the join is instant because the worker has already left. (c) `TrySkipStartupCatalogFetch` parses the view's JQL for a project on the UI thread: a pure string parse (Plane, whose resolver can call the network, is skipped by design).
  - *Findings outside the scope (not changed).* `chat_persist::Start(*Cache, ...)` (`AppController_Init.cpp`) keeps a raw `LocalCacheManager&` for the process lifetime, so after any cache recreate (old or new path) the AI chat persist worker writes through a dangling reference; it needs the latched `shared_ptr` or a re-bind. `AppController::GetAvailableFields()` returns a reference into the catalog without its lock while workers can write it (`RefreshFieldCatalog`).
- **Session stats are approximate (kept as is).** `sync.stats` samples a shared tracker traffic counter, so two panes syncing at once cross-count requests and bytes. Decision: accept and say so. The `sync.stats` command description and `docs/guides/cli.md` call the request and byte numbers approximate; there is no per-session attribution.
- **`LookupCacheTtlSec` stays 0 (off).** A picker opened online, or reopened after an outage, reloads live (the offline-first contract). No Preferences control, no non-zero default.
- **`ApplyIssueFetchPack` is a documented test-only seam.** `TicketSyncService::ApplyIssueFetchPack` stays (its header says it has no production caller and that production applies through the streaming path); the unused `AppController::ApplyIssueFetchPack` passthrough was deleted. No test was removed.

## Phase 7 notes (lookups)

- `FieldCatalogRefreshTtlSec` (default 43200) and `LookupCacheTtlSec` (default 0 = off; opt in with 60..2592000) are `config.set` keys only; no Preferences control yet.
- The saved project list and the saved project components skip the live fetch while younger than `LookupCacheTtlSec` (only when it is set above 0: the default 0 keeps the offline-first contract that a picker opened online, or reopened after an outage, reloads live); the per-issue-type edit permissions (`kEditMetaTypeKind`) still refresh on every warm and are a follow-up.
- The startup catalog fetch is skipped once per run when the restored snapshot is inside its lifetime and covers every column of the active view; the grid shows it with a "saved" cue and refetches when it expires. `FieldCatalogCache` now keeps `savedUnix` in the index (the v3 load also used to drop the whole `entries` index on every read; fixed).
- `ITicketSyncDeps::OnTicketsRemotelyChanged` fires for resident rows whose `updated` moved (sync drain and change-monitor apply) and invalidates their transitions and per-issue editmeta.
- The comments modal shows the saved thread instantly as a seed and always runs the live fetch (the Phase 7 refetch skip and its `comment_thread_at` stamp were removed in the pre-merge review: a stamp from the same cached row says nothing about the tracker).

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
