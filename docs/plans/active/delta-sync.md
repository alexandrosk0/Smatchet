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
| 6 | `feat/delta-sync-p6-monitor` | change monitor uses one roster call (`DiffRosterAgainstResident`); new keys join the grid |
| 7 | `feat/delta-sync-p7-lookups` | field-catalog snapshot TTL; transitions/editmeta invalidation on remote change; `lookup_cache` TTL; comments modal skips refetch for an unchanged ticket |

### Fallback matrix

| Condition | Action |
|---|---|
| Forced, delta disabled, backend has no roster support, or no meta yet | Full |
| Roster error | Full |
| Roster incomplete (cap hit or warning) | Delta for the rows seen; no stale purge |
| More than 50% changed and more than 100 rows | Full |
| A body batch is refused | Isolate the key; treat it as gone |
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

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
