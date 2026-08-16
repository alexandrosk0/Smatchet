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

## Verification results

- `cmake --build --preset ninja-test-msvc` → BUILD=0; `ctest --output-on-failure` → **7/7 passed**
  (`smatchet_tests` 33.7 s incl. the 4 new cases, `smatchet_lua_tests`, 5 android-openssl lanes).
- `cmake --build --preset ninja-iter-msvc` → BUILD=0 (app relinks clean).
- `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` → **exit 0**, all gates PASS.
  Remaining WARNs are soft-tier and pre-existing in kind: `ApplyIssueFetchPack` 101L > 100 soft
  target, comment-ratio on the three touched headers.
- Manual 3-pane check: still to run against the populated cache in `C:\Dev\Smatchet\build\`.

## Deviations

- none

## Follow-ups surfaced by the audit (out of scope here)

A read-only audit for other "shared state treated as per-pane" defects found several that are
adjacent to this fix but independent of it. Two interact with the retention set directly:

- **`RefreshLocalData` repopulates a pane from the whole namespace**
  (`AppController_CatalogAndFieldEdit.cpp:56`, `ctx.ActiveTickets = Cache->GetAllTickets(cacheKey)`).
  Previously masked because the sibling sweep deleted the other pane's rows; retaining them makes
  a post-edit refresh show the union of both panes' queries.
- **Hidden-pane eviction / retirement clears `ActiveTickets`**
  (`AppController_PaneContexts.cpp:708`, `:810`), which drops those rows out of the retention set,
  so the next focused-pane full sync deletes them from `tickets_v2`.

Neither is caused by this change, but both should land before multi-pane is considered correct.
