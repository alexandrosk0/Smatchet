# Pane stale-delete collision — multi-pane full syncs wipe each other's cache rows

**Status**: active
**Owner**: orchestrator (Sync subsystem — `offline-sync` scope)
**Tier**: fix

## Problem

With more than one grid pane live, each pane owns its own `GridLiveContext` and therefore its
own `TicketSyncService`, but every pane writes into **one** SQLite cache namespace
(`LocalCacheManager` rows keyed by `CacheBackendKey()`, e.g. `"Jira"`).

`TicketSyncService::RunStreamingWorkerBody` computes the stale set as
*every cached id in the namespace that this session's query did not return*
(`GetAllTicketIds(backendKey)` minus `workerKeepIds`). That is only correct when the query
covers the whole namespace. With N panes running N different JQLs it means each pane deletes
every row the other panes are displaying.

Observed live (`Smatchet-3192.log`, 3 panes):

```
pane A: saved_or_kept=48  total_stale=32  fullSync=1 -> stale deletion total_deleted=32
pane B: saved_or_kept=32  total_stale=48  fullSync=1 -> stale deletion total_deleted=48
AppController::TickChangeMonitors reconcile capped pane='main': 48 vanished, probing 25 this cycle
```

Steady-state thrash every ~120 s; whichever pane synced last is the only one with rows, the
rest render empty.

The existing `ShouldSkipMassDeletionOnEmptyFullSync` guard only defends against a *zero-result*
full sync. It cannot see a non-empty full sync from a differently-scoped sibling pane.

## Approach

Subtract the ids other live panes are currently holding from the stale set before deleting.

Chosen over the alternatives:
- *Per-pane cache namespace* — wrong: the cache is deliberately backend-scoped and shared
  (ADR-0018 decision 4); per-pane namespacing would duplicate every row N times and break the
  offline queue's namespace contract.
- *Only the focused pane may stale-delete* — leaves non-focused panes' deletions permanently
  disabled and still wipes the focused pane's rows when focus moves.

Subtraction keeps the "row vanished from the remote" semantics for rows nobody is showing,
while never deleting a row a live pane is currently rendering. Worst case a genuinely-deleted
remote row lingers one extra sync cycle in a sibling pane, which that pane's own next full sync
removes.

## Files to modify

| File | Change |
|---|---|
| `Source/Core/include/ITicketSyncDeps.h` | new defaulted `TicketIdsRetainedByOtherContexts()` hook |
| `Source/Core/include/TicketSyncService.h` | declare pure static `FilterStaleIdsRetainedElsewhere` |
| `Source/Core/src/Sync/TicketSyncService.cpp` | implement helper; apply at both stale-delete seeds (streaming + `ApplyIssueFetchPack`) |
| `Source/Core/include/GridContextDepsAdapter.h` / `.cpp` | override forwarding to AppController |
| `Source/Core/include/AppController.h` | declare `CollectTicketIdsRetainedByOtherContexts` |
| `Source/Core/src/AppController_PaneContexts.cpp` | implement the collector over `gridContexts_` |
| `tests/Core/TicketSyncService.test.cpp` | pure-helper cases + a deps-driven collision case |

Second slice — the eight audit findings (originally listed as follow-ups, now in scope; see
§ Audit findings F1-F8):

| File | Change |
|---|---|
| `Source/Core/include/Sync/TicketRosterFilterPure.h` | **new** header-only `RetainTicketsInKeepSet`, shared by the Sync + AppController sides |
| `Source/Core/include/Sync/TicketSyncService.h` / `src/Sync/TicketSyncService.cpp` | `ConvergeActiveTicketsToSessionKeepSet` — a pane's roster converges onto its OWN query, then publishes its owned ids |
| `Source/Core/include/ITicketSyncDeps.h` | defaulted `PublishOwnedTicketIds()` hook |
| `Source/Core/include/IEditMetaDeps.h` | defaulted `GetActiveTicketsSnapshotsAllPanes()` hook |
| `Source/Core/include/GridContextDepsAdapter.h` / `.cpp` | both overrides; the adapter resolves its OWN context (no focus-following) |
| `Source/Core/include/AppController.h` | `paneOwnedTicketIds_` + `paneOwnedIdsMutex_`, Set/Get/Forget/Drop, `CollectActiveTicketSnapshotsAcrossContexts`, `admitId` on `RefreshLocalDataCheckedImpl_` |
| `Source/Core/src/AppController_PaneContexts.cpp` | owned-id store; the 404 purge skips ids a sibling still retains; `DropPaneOwnedTicketIds` |
| `Source/Core/src/AppController_CatalogAndFieldEdit.cpp` | `RefreshLocalData` scopes the namespace-wide read to the pane's owned ids (+ `admitId`) |
| `Source/Core/src/EditMetaCacheService.cpp` | editmeta prune unions every live pane's roster |
| `Source/Core/src/SmatchetTicketChangeNotifications.cpp` | toast dedup signature keyed per pane |
| `tests/Core/TicketRosterFilterPure.test.cpp` + `tests/CMakeLists.txt` | 4 doctest cases for the shared filter |

## Threading

`TicketIdsRetainedByOtherContexts()` is called on the **UI thread** only, from the
stale-deletion seed in `FinalizeStreamingSessionIfDone` / `ApplyIssueFetchPack` — never from
the streaming worker. It is computed **before** `activeStreamingSync_.QueueMutex` is taken:
the collector locks `gridContextsMutex_` then per-context `activeTicketsMutex_`, and
`DrainPendingStreamingBatches` already takes `activeTicketsMutex_` -> `QueueMutex`, so taking
`activeTicketsMutex_` under `QueueMutex` would invert that order.

The collector follows the documented issue-#1457 lock discipline: snapshot the context
pointers under `gridContextsMutex_`, release it, then take each per-context
`activeTicketsMutex_` — never both at once. Retired contexts stay alive as ADR-0012 husks so
the snapshotted pointers cannot dangle.

## Perf gate

Touches `Source/Core/`. The added work is **once per completed full-sync session** (not
per-frame): one pass over the live panes' `ActiveTickets` (O(total rows across panes), tens to
low thousands of ids) plus one hash-set build over the stale ids. The per-frame stale-deletion
budget loop (3 ms / 10 ids) is unchanged. No new allocation on the UI hot path, no new work in
`TickStreamingApply`'s idle path. No perf-scenario baseline shift expected.

## Verification

- `ctest` — new pure-helper cases + the deps-driven collision case.
- Manual: 3 panes with different JQLs, watch the log for
  `stale deletion total_deleted=` no longer reaching the sibling panes' row counts, and
  `retained by N other live pane(s)` accounting for the difference.

## Implementation log

- `ITicketSyncDeps::TicketIdsRetainedByOtherContexts()` added as a defaulted-empty hook, so the
  test fakes and any single-context deps implementation keep the pre-fix behaviour without a stub.
- `TicketSyncService::FilterStaleIdsRetainedElsewhere` implemented as a pure static
  (hash-set over the retention list, order-preserving `copy_if`, early-out when the retention
  set is empty). Applied at **both** stale-delete seeds — `ApplyIssueFetchPack`'s delete loop
  and the streaming session finalizer.
- `AppController::CollectTicketIdsRetainedByOtherContexts` walks `gridContexts_`, skipping
  `self` and any context in a different cache namespace, and returns their `ActiveTickets` ids.
  `GridContextDepsAdapter` forwards the hook to it.
- The backend-key filter deliberately runs **outside** the `gridContextsMutex_` scope:
  `CacheBackendKeyCopy()` takes `backendKeyMutex_`, and issue #1457 forbids holding the map
  mutex while taking any per-context mutex.
- Extracted the stale-deletion seed out of `FinalizeStreamingSessionIfDone` into
  `SeedStaleDeletionForSession(fullSyncCompleted, keptThisSession)` — the added lines pushed the
  finalizer to 134 lines, over the 120-line `function-too-long` cap.

Second slice (F1-F8):

- The owned-id map is keyed by `backendKey|paneId` **strings**, not context pointers: a retired
  ADR-0012 husk keeps its pointer alive but a recreated pane for the same id must inherit the
  recorded set, and a string key survives both.
- `paneOwnedIdsMutex_` is the **innermost** mutex — nothing else may be taken while holding it.
  `ConvergeActiveTicketsToSessionKeepSet` therefore collects the ids under
  `activeTicketsMutex_`, releases it, and only then calls `PublishOwnedTicketIds`: the adapter
  reads its context's backend key under `backendKeyMutex_`, and `activeTicketsMutex_` must stay
  the innermost of that pair.
- `RetainTicketsInKeepSet` was extracted into `Sync/TicketRosterFilterPure.h` because the
  identical erase-remove-if landed on both sides of the scoping and tripped the blocking
  `duplication` gate (72-token clone, `AppController_CatalogAndFieldEdit.cpp` ↔
  `TicketSyncService.cpp`). Extraction was chosen over a `SMATCHET_DEVIATION(rule=duplication)`
  escape — the two sites are genuinely the same operation, and a header-only pure function is
  unit-testable without either TU's dependencies.
- The F2 pane-scoping filter deliberately **skips** when the recorded set is empty. Filtering an
  empty set would blank a pane that has not yet completed its first sync, which is exactly the
  empty-grid symptom this plan exists to remove.

## Verification results

- `cmake --build --preset ninja-test-msvc` → BUILD=0; `ctest --output-on-failure` → **7/7 passed**
  (`smatchet_tests` 33.7 s incl. the 4 new cases, `smatchet_lua_tests`, 5 android-openssl lanes).
- `cmake --build --preset ninja-iter-msvc` → BUILD=0 (app relinks clean).
- `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` → **exit 0**, all gates PASS.
  Remaining WARNs are soft-tier and pre-existing in kind: `ApplyIssueFetchPack` 101L > 100 soft
  target, comment-ratio on the three touched headers.
- Manual 3-pane check: still to run against the populated cache in `C:\Dev\Smatchet\build\`.

Second slice (F1-F8), re-run after every finding landed:

- `cmake --build --preset ninja-iter-msvc` → BUILD=0 (app relinks clean).
- `cmake --build --preset ninja-test-msvc` → BUILD=0; `ctest --output-on-failure` → **7/7 passed**
  (35.01 s), including the 4 new `RetainTicketsInKeepSet` cases.
- `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` → all gates **PASS**
  after the `TicketRosterFilterPure.h` extraction (the `duplication` gate failed before it).
  Remaining WARNs are soft-tier and pre-existing in kind: `func-size` on `ApplyIssueFetchPack`,
  comment-ratio on four touched headers.
- Manual multi-pane check: still to run — the automated coverage is unit-level only, so the
  cross-pane behaviour (no sibling wipe, no cross-pane row leak into a refreshed pane, per-pane
  toasts) needs one 3-pane session against a populated cache.

## Deviations

- **Both slices ship on one PR (#2024).** The repo's PR-batching rule prefers one PR per logical
  feature; the audit findings were opened as a separate follow-up list. They were folded into
  this PR on explicit user instruction. The technical justification is that slice 1 alone is not
  shippable-correct — retaining a sibling's rows converts "rows deleted" into "rows leak across
  panes" (F2/F3), so the two slices are one behavioural change split only by discovery order.

## Audit findings F1-F8 (second slice — implemented here)

A read-only audit for other "shared state treated as per-pane" defects found eight. All eight
are fixed on this branch; the retention fix above alone left the multi-pane path incorrect
(retaining a sibling's rows only moved the symptom from "rows deleted" to "rows leak across
panes"), so they ship together.

- **F1 — `GridContextDepsAdapter` followed the focused pane.** Each adapter now resolves its OWN
  latched context, and each context draws from its own 2^32 generation band
  (`NextBackendGenerationSeed()`), so a background pane's sync can no longer be attributed to —
  or invalidated by — whichever pane happens to be focused (issue #1081 discipline).
- **F2/F3 — `RefreshLocalData` repopulated a pane from the whole namespace.**
  `Cache->GetAllTickets(cacheKey)` returns the union of every pane's query (ADR-0018 decision 4).
  The read is now narrowed to the ids this pane recorded as its own. Two deliberate carve-outs:
  an **empty** recorded set falls back to the whole namespace (cold start — a pane that has never
  completed a sync must still render), and `UpdateTicket` passes the just-saved row's id as
  `admitId` so the row it wrote survives a filter built before the save.
- **F4 — nothing recorded what a pane owns.** Added `paneOwnedTicketIds_`
  (`backendKey|paneId` → ids) behind the innermost `paneOwnedIdsMutex_`, written from
  `ConvergeActiveTicketsToSessionKeepSet` → `PublishOwnedTicketIds` → the adapter, unioned into
  `CollectTicketIdsRetainedByOtherContexts`. This also closes the original follow-up about
  hidden-pane eviction clearing `ActiveTickets`: the recorded set outlives the in-memory roster,
  so an evicted pane's rows stay in the retention set and survive a sibling's full sync.
- **F5 — the editmeta prune only saw one pane's roster.** It now unions every live pane's roster
  via `CollectActiveTicketSnapshotsAcrossContexts` / `GetActiveTicketsSnapshotsAllPanes`;
  previously a process-wide cache was pruned against a single pane's view and evicted entries a
  sibling was actively using.
- **F6 — documentation only**, resolved by F1; recorded here rather than as a separate change.
- **F7 — the toast dedup signature was process-wide.** One shared `s_lastSignature` let two panes
  polling different queries cancel each other out: pane B's batch overwrote the key pane A would
  next compare against, so A's genuine repeat slipped through while B's real change was suppressed
  whenever the two batches matched. Now keyed per pane.
- **F8 — the 404 reconcile purged shared cache rows unconditionally.** A verdict now skips any id
  a sibling pane still retains (that sibling reaches the same 404 on its own poll, so the row
  still converges away), and the purged ids are dropped from this pane's recorded set so it stops
  whitelisting — and pinning — dead ids.
