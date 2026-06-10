<!-- index-summary: Fix #1081 backend-switch (Jira→GitHub) `std::terminate` — publish-under-lock, `backendGeneration_` token gating stale-worker writes + replay-refresh TOCTOU re-check, captured-key replay latch, and `PaneSyncKickPolicy` sync-storm damping. -->
# backend-switch-sync-race-1081 — fix plan

Fixes GitHub issue #1081: **crash: backend switch (Jira→GitHub) races in-flight sync workers → `std::terminate`**. Diagnosis by debug-detective (trusted, not re-derived here): the backend-swapped branch of `TicketSyncService::SwapBackendIfTrackerChanged` publishes the empty ActiveTickets snapshot OUTSIDE `ActiveTicketsMutex()` (finding A — shared_ptr control-block UB against worker-thread writers/readers, primary terminate candidate), and in-flight workers (offline replay completions, `UpdateTicket`, the `EnsurePaneLiveSyncStarted` main-thread post) apply stale-backend results into the post-switch context (finding B — TOCTOU contamination + stale-cfg backend flip-flop), amplified by an unbounded frame-rate sync-retry storm on fast-fail (`OnStreamingSyncSessionFinished(fetchOk=false)` re-arms `initialSyncKicked` every failed session).

## Fixes

1. **Lock fix** — widen the `ActiveTicketsMutex()` scope in the backend-swapped branch so clear + `SetActiveTicketsPublished` happen under ONE lock (contract: `GridLiveContext.h:71-75`). Comment-enforce the must-hold-mutex contract at `GridContextDepsAdapter::SetActiveTicketsPublished`.
2. **Backend-generation token** — `std::atomic<std::uint64_t> backendGeneration_{0}` on `GridLiveContext`; bumped in `GridContextDepsAdapter::SetBackend` (after the atomic_exchange) and in `AppController::retireExpiredHiddenContexts_`. Cancel/await was REJECTED by the diagnosis (UI-thread block up to an HTTP timeout — Pillar 2).
3. **Capture-then-check apply sites** — capture generation (+ cache backend key) at work-capture time; re-check before mutating; on mismatch drop the apply + `LOG_INFO`:
   - `AppController::RefreshLocalData` gains a generation-checked overload (check under `activeTicketsMutex_`); UI-thread callers keep the unchecked path.
   - `AppController::UpdateTicket` latches key+generation once at entry; skips `SaveTicket` + refresh on mismatch.
   - Offline replay (`TickOfflineCreates` / `TickOfflineFieldEdits`): capture generation + backend key at tick time; gen-check before `RefreshLocalData`; pass the CAPTURED key into `IssueCreatePipeline::Run` (replaces the write-time `depsRef.CacheBackendKey()` re-read). New `IOfflineQueueDeps::BackendGeneration()`.
   - `EnsurePaneLiveSyncStarted`: capture generation at kick; the main-thread post drops BOTH `SyncPaneWithBackend` (stale-cfgCopy backend flip-flop) and the cache seed on mismatch, and **re-arms `initialSyncKicked`** so the pane re-kicks with fresh state instead of dead-latching (necessary consequence of dropping the kick; flagged in §Deviations).
   - `TicketSyncService` internal drains: NO token (already serialized — worker joined before swap; drains UI-thread).
4. **Sync-storm damping** — `syncRetryAfter` time-point on `GridLiveContext` (UI-thread-only, same discipline as `initialSyncKicked`); set `now + 30 s` in the `fetchOk=false` branch; `EnsurePaneLiveSyncStarted` bails inside the window via the pure helper `smatchet::ShouldKickInitialSync(now, kicked, retryAfter)` (header-pure, `BackgroundWorkerReap.h` precedent; mirrors the `projectComponentsRetryAfter_` 30 s pattern).

## Files-to-modify

| File | Change |
|---|---|
| `Source/Core/include/GridLiveContext.h` | add `backendGeneration_` atomic + `syncRetryAfter` time-point |
| `Source/Core/include/PaneSyncKickPolicy.h` | NEW — pure kick/apply decision helpers (`ShouldKickInitialSync`, `PaneSyncKickStillCurrent`) |
| `Source/Core/include/IOfflineQueueDeps.h` | add `BackendGeneration()` |
| `Source/Core/include/GridContextDepsAdapter.h` | declare `BackendGeneration()` override |
| `Source/Core/src/GridContextDepsAdapter.cpp` | gen bump in `SetBackend`; `syncRetryAfter` in `OnStreamingSyncSessionFinished`; publish-contract comment; `BackendGeneration()` |
| `Source/Core/src/Sync/TicketSyncService.cpp` | Fix 1 — publish under the lock |
| `Source/Core/src/Sync/OfflineQueueService.cpp` | capture key+gen at tick; gen-check before refresh; captured key through `ReplayOneCreate` → `IssueCreatePipeline::Run` |
| `Source/Core/include/Sync/OfflineQueueService.h` | `ReplayOneCreate` signature (+ captured key param) |
| `Source/Core/src/AppController.cpp` | gen capture/check + retry-window bail in `EnsurePaneLiveSyncStarted`; gen bump in `retireExpiredHiddenContexts_` |
| `Source/Core/src/AppController_CatalogAndFieldEdit.cpp` | `RefreshLocalData` gen-checked overload; `UpdateTicket` key+gen latch |
| `Source/Core/include/AppController.h` | declare the overload |
| `tests/support/FakeOfflineQueueDeps.h` | `BackendGenerationImpl` member + override |
| `tests/support/FakeTicketSyncDeps.h` | publish-under-lock recorder (cross-thread try_lock probe — NOT same-thread try_lock) |
| `tests/Core/BackendSwitchRace1081.test.cpp` | NEW — tests 1–5 below |
| `tests/CMakeLists.txt` | register the new test TU |

## Perf-gate (mandatory — diff touches Source/Core/)

All added steady-state work is O(1): the generation token is a relaxed-cost `std::atomic<uint64_t>` load at work-capture/apply sites (off the per-frame hot path — replay ticks, swap path, one-shot kick), the storm-damping check is one `steady_clock` compare on the already-early-out `EnsurePaneLiveSyncStarted` path, and Fix 1 only widens an existing lock scope by one shared_ptr assignment (swap path, not per-frame). No new allocation, no new locks, no per-frame cost. Net: the storm fix REMOVES unbounded frame-rate re-sync work. No perf gate run needed beyond the standard suite.

## Verification

- Build: `cmake --build --preset ninja-iter-msvc`.
- Tests: `scripts/dev/test-all.sh` (or ctest preset) — new doctest cases:
  1. Publish-under-lock contract (fails pre-Fix-1, passes post) via instrumented `FakeTicketSyncDeps` + `SyncWithBackend` backend-kind change.
  2. Generation drop: gen bumped between capture and apply → `RefreshLocalData` not called; control case (no bump) → called.
  3. Storm damping: `ShouldKickInitialSync` decision matrix (inside window → no kick; past window → kick; latched → no kick).
  4. Stale-cfg no-reswap decision: `PaneSyncKickStillCurrent` (context-gone / gen-moved → drop) — AppController wiring itself is not unit-reachable, see §Deviations.
  5. Replay key latch: queued row under key "Jira", key+gen switched mid-replay (deferred `BackgroundTaskRunner`) → write lands under "Jira", absent under "GitHub", refresh skipped.
- Lint: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` + `clang-format -i` on touched C++.

## Implementation log

- Fixes 1–4 implemented exactly per the §Fixes plan; all §Files-to-modify rows touched, plus one extra row (below) for a lint-cap decomposition.
- `EnsurePaneLiveSyncStarted` crossed the `function-too-long` cap (127 L > 120) after the capture-then-check + storm-damping additions → main-thread completion body extracted to private `AppController::applyPaneSyncKickOnMainThread_` (declared in `AppController.h`); behaviour unchanged, lint gates green afterwards.
- `cmake --build --preset ninja-iter-msvc` → PASS (full rebuild after fresh configure; MSVC env via `scripts/dev/with-msvc-env.sh`).
- `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` → all gates PASS; WARNs only (soft-tier func-size on pre-existing `ReplayOneCreate` growth to 104 L, comment-ratio on doc-comment-heavy headers, one pre-existing dup calibration WARN).
- New doctest TU `tests/Core/BackendSwitchRace1081.test.cpp` (6 TEST_CASEs covering verification items 1–5; item 2 has an explicit control case) built + run via the `ninja-test-msvc` rig.

### PR #1104 review round 1 (internal code-review findings)

- **HIGH (TOCTOU re-open in replay refresh)**: the post-replay generation check was worker-side only — the slow `GetAllTickets` full-table read sat between the check and the locked `ActiveTickets` replace. Added `IOfflineQueueDeps::RefreshLocalData(std::uint64_t capturedBackendGeneration)`; `GridContextDepsAdapter` forwards its latched `ctx_` (friend access) into `AppController::RefreshLocalDataCheckedImpl_`, which re-checks the generation under `activeTicketsMutex_` immediately before the swap-in. Cache read stays OUTSIDE the mutex (no nested SQLite-under-tickets-mutex); the worker pre-check survives as a cheap skip of the read. Both `OfflineQueueService` replay sites (field-edit + create ticks) now call the checked overload.
- **MEDIUM-1 (cross-context generation comparison)**: `RefreshLocalDataCheckedImpl_` re-resolved `focusedContext()` at apply time — per-context counters equal-by-coincidence could pass the gate while focus moved. Impl now takes `GridLiveContext&`; the ctx-less public checked overload `RefreshLocalData(uint64_t)` was REMOVED (it was the footgun) — checked callers name their latched context (`UpdateTicket` inline, replay workers via the friend adapter). Lifetime relied upon: retired contexts park as defer-free husks in `retiredContexts_` until `~AppController` (documented at both call sites).
- **MEDIUM-2**: `UpdateTicket` comment tightened (post-check swap is benign — write lands under the captured key where the row belongs; the check does NOT close the pre-SaveTicket window) and the drop-path log upgraded INFO→WARN (backend mutation already succeeded upstream; drop = stale row until old backend's next sync), with key/ticket/generation values.
- **LOW-1**: `applyPaneSyncKickOnMainThread_` drop path now clears `lastSyncedJql` alongside the `initialSyncKicked` re-arm (GridLiveContext.h "cleared with the latch" discipline).
- **LOW-2**: `RefreshLocalData` skip log now carries key + captured/current generation values.
- New regression TEST_CASE (7th): generation moves MID-refresh (after worker pre-check, during the cache-read window) → checked overload drops the swap-in; `FakeOfflineQueueDeps` grew the checked override with production drop semantics + `OnCheckedRefreshLocalData` entry hook + drop counters.
- Verification: `ninja-test-msvc` (SmatchetTests) + `ninja-iter-msvc` (full app, compiles the AppController TUs the test rig doesn't) both PASS; `*1081*` 7/7; full rig 1524/1524 (14966 assertions); lint gates PASS (soft WARNs only, pre-existing).

## Deviations

- **`initialSyncKicked` re-arm on stale-kick drop** (Fix 3, `applyPaneSyncKickOnMainThread_`): when the generation check drops a kick, the one-shot latch is reset to `false` — without this the pane dead-latches unsynced forever (the latch was set at kick time but no sync ever applied). Not in the original spec text; necessary consequence of dropping the kick.
- **Fixes 3-partial/4 tested via extracted pure helpers** (`PaneSyncKickPolicy.h`: `ShouldKickInitialSync` / `PaneSyncKickStillCurrent`) because `AppController` has no unit-test rig — explicitly permitted by the spec (BackgroundWorkerReap.h precedent). The stale-cfg no-reswap behaviour is the `PaneSyncKickStillCurrent == false` branch dropping `SyncPaneWithBackend`.
- **Test 2 asserts via the fake's `RefreshLocalDataCalls` counter** as the proxy for "ActiveTickets/snapshot/revision unchanged": in production `RefreshLocalData` is the only thing that replaces ActiveTickets + republishes + bumps the revision on this path, so not calling it is equivalent.
- **`ITicketSyncDeps` intentionally NOT given `BackendGeneration()`**: `TicketSyncService` internal drains are already serialized vs the swap (worker joined first; drains UI-thread) — per spec Fix 3 note.
