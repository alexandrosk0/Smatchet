# Plan — large central files + transitional friend access
<!-- index-summary: Large-file split (Track A) + service-extraction Phase 1 (Track B). Track A fully shipped; Track B Phase 1A-1D shipped, Phase 2 interface bundles deferred — see plan § Implementation log. -->

## Context

A reviewer flagged two related code-health issues:

1. **Large central files.** Six files dominate the codebase by line count:
   - `Source_Core/src/AppController_LuaBindings.cpp` — 2648 LOC
   - `Source_Core/src/Commands/BuiltinCommands.cpp` — 1898 LOC
   - `Source_Core/src/ConfigManager.cpp` — 1773 LOC
   - `Source_Core/src/PlaneClient.cpp` — 1646 LOC
   - `Source_Core/src/SmatchetUI.cpp` — 1454 LOC
   - `Source_Core/include/AppController.h` — 1035 LOC

   Each one slows review, hides ownership, and gates merges behind unrelated diff lines.

2. **Transitional architecture.** `AppController.h` has three `friend class` declarations to services extracted in incomplete refactors:
   - `friend class OfflineQueueService;` (L94) — item 12 Phase 1A-1D shipped, Phase 2 (interface bundle) deferred
   - `friend class TicketSyncService;` (L99) — item 11 Phase 1C shipped, Phase 2 deferred
   - `friend class LuaAutomationHost;` (L104) — item 14 Phase 1A shipped, Phase 1B-1D + Phase 2 deferred

   Comments reference `BACKLOG_CODE_REVIEW.md §1.7 / §7 item N` but **that doc no longer exists** — the references are stale. Phase 2 plan ("small set of interface bundles") was never materialised.

**Goal.** Reduce each big file to a reviewable size (≤ ~800 LOC target) and finish the Phase 2 cleanup so no extracted service needs `friend` access. Net result: smaller files, no transitional shims, no dangling backlog references.

---

## Approach — two independent tracks, parallelisable

### Track A — mechanical file splits (low risk, lands first)

One PR per file. No semantic change. Each split follows the `BlameAnalysisUi` precedent already in the tree (`BlameAnalysisUi_Window.cpp`, `_Internal.h`, `_Modals.cpp`, …).

#### A1 · BuiltinCommands.cpp → per-category files

Cleanest split. The file is one big `RegisterBuiltinCommands` with `=== category ===` dividers already marking the boundaries:

```
meta, app, config, perf, tickets, debug, sync, ticket (mutations),
fields, users, offline, scenario, ui_test, attach
```

Pattern: one `Register<Cat>Commands(CommandRegistry&, AppController&)` per category file, called from `RegisterBuiltinCommands` (which becomes a 30-line dispatcher). Mirrors existing `RegisterViewToggleCommands(reg, app)` call at `BuiltinCommands.cpp:1892`.

Files created in `Source_Core/src/Commands/Builtin/`:

- `BuiltinCommands_Meta.cpp` (commands.list / help / search / recents)
- `BuiltinCommands_App.cpp`
- `BuiltinCommands_Config.cpp`
- `BuiltinCommands_Perf.cpp`
- `BuiltinCommands_Tickets.cpp`
- `BuiltinCommands_Debug.cpp`
- `BuiltinCommands_Sync.cpp`
- `BuiltinCommands_Fields.cpp`
- `BuiltinCommands_Users.cpp`
- `BuiltinCommands_Offline.cpp`
- `BuiltinCommands_Scenario.cpp`
- `BuiltinCommands_UiTest.cpp`
- `BuiltinCommands_Attach.cpp`
- `BuiltinCommands_Internal.h` (shared `MakeCommand`, `PString`, `PInt`, `PaginateJsonArray` helpers — currently in anon namespace)

Add new sources to `CMakeLists.txt` `target_sources(SmatchetCore_DX12 …)` + `target_sources(SmatchetStandalone …)`. Use `command-system` agent.

Expected size: each file 100–250 LOC. Top file `BuiltinCommands.cpp` drops to ~80 LOC dispatcher.

#### A2 · PlaneClient.cpp → Search / Mutation / Catalog

Mirror the existing `JiraIssueSearch.cpp` / `JiraIssueMutation.cpp` / `JiraUserAndMeta.cpp` split (the same precedent is one floor up). Splits:

- `PlaneClient.cpp` (constructor, shared state, `BuildPlaneHeaders`, anon-namespace helpers)
- `PlaneIssueSearch.cpp` (`FetchIssues`, `FetchIssuesStreamed`, `FetchIssuesForKeys`, `ProbeReachability`, `ExtractProjectFromQuery`, `ListProjects`, `InvalidateListProjectsCache`)
- `PlaneIssueMutation.cpp` (`UpdateIssueFields`, `BuildFieldPayload`, `UpdateField`, `ResolveDisplayValue`, `CreateIssue`, `BuildCreatePayload`, `BuildUpdatePayload`, `AttachFilesToIssue`, `AddIssueToSprint`)
- `PlaneFieldCatalog.cpp` (`FetchFieldCatalog`, `FetchIssueEditMeta`, `BuildBrowseUrl`)

Use `tracker-backend` agent. Closure rule: every callee of the named entry-points lands in the same bucket; shared helpers (anon namespace) stay in the root `PlaneClient.cpp`.

Expected size: 4 files, each ~250–500 LOC.

#### A3 · ConfigManager.cpp → I/O helpers + Views + main

Three buckets:

- `ConfigManager.cpp` (`Save`, `SaveBlameAnalysis`, `Load`, `GetConfigPath`, `GetViewsPath`, `NormalizeUiLanguageCode`, env-cache reset, storage-preference flag)
- `ConfigManager_Views.cpp` (`SavePersistentViewsToDisk`, `EnsureViewBucketBootstrapped`, `ViewsStoreToViewWorkspace`, `NormalizeViewsBackendKey`, `WriteConfigJson`, `to_json` / `from_json` for `CommentTemplate`)
- `ConfigManager_PathUtils.cpp` (filesystem helpers — `Utf8ToWide`, `EnsureDirectoryExists`, `CreateDirectories`, `EnsureParentDirectoryForFile`, `FileExists`, `NormalizeDirectoryPath`, `AtomicWriteTextFile`, base64 codec, secret-protect glue, `WriteDefaultImGuiSettingsFile`, `EnsureDefaultImGuiSettingsFile`)
- `ConfigManager_Internal.h` (anon-namespace state — `GetIoMutexRef`, `GetCacheMutexRef`, `GetHasCachedConfigRef`, `GetRuntimeAssetDirectoryRef`, `GetUserDataDirectoryRef`, `ViewsStoreToViewWorkspaceImpl`)

Use orchestrator (no specialist agent for `ConfigManager`). Expected size: 3 files, each ~400–700 LOC.

#### A4 · SmatchetUI.cpp → MainMenu + Layout + Draw

Three buckets:

- `SmatchetUI.cpp` (`Draw`, `drawEnsureCatalogAndInitialSync`, private state)
- `SmatchetUI_MainMenu.cpp` (`drawMainMenuBar` ~449 LOC, only call site is `Draw`)
- `SmatchetUI_Layout.cpp` (`prepareTopLevelWindow`, `repairTopLevelWindow`, `resetWindowLayoutToDefault`, `SmatchetUI_ResetLayoutToDefault`, `DrainUiDrawSessionFuturesBeforeAppTeardown`)

Use `grid-engine` agent (it owns the dashboard / UI shell). Internal header `SmatchetUI_Internal.h` shares any helpers the MainMenu and Layout files both need. Expected size: 3 files, each ~250–700 LOC.

#### A5 · AppController.h → split Lua nested types to companion header

Modest gain. Extract the Lua nested types (`ImCmd`, `LuaFieldCacheEntry`, `LuaWindowEntry`, `PendingLuaWindowOp`) at lines 788–887 into `AppController_LuaTypes.h`, guarded by the same `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` block. `AppController.h` includes it.

Drops `AppController.h` to ~880 LOC. Use `lua-binder` agent.

#### A6 · AppController_LuaBindings.cpp — DEFER

Do **not** split this file. It is already destined to dissolve into `LuaAutomationHost.cpp` in Phase 1B (`InitLua`), 1C (Lua bindings), 1D (worker thread). Splitting it now creates files that will be re-deleted in Track B. Recommend: skip A6, do the LuaAutomationHost Phase 1B-1D migration directly under Track B.

### Track B — finish Phase 2 (drop friend classes)

Replace each `friend class` with a narrow interface that the service consumes by reference. The interface is implemented by AppController. Goal: services depend on a small interface, not the full AppController surface; the friendship goes away.

#### B1 · OfflineQueueService — `ICacheAccess` interface

Friend surface is small: `app_.Cache->…` (`LocalCacheManager`) is essentially the entire dependency. The natural interface bundle is just **a `LocalCacheManager*` accessor**:

```cpp
class ICacheAccess {
  public:
    virtual ~ICacheAccess() = default;
    virtual LocalCacheManager* Cache() = 0;
};
```

AppController inherits it (or wraps to expose a small adapter). `OfflineQueueService` ctor takes `ICacheAccess&` instead of `AppController&`. Drop `friend class OfflineQueueService;`.

Use `offline-sync` agent.

#### B2 · TicketSyncService — `ITicketSyncHost` interface

Largest friend surface (~17 distinct private member references). Inventory from this investigation:

- `app_.Cache`, `app_.Backend`, `app_.backendFactory_` — cache + backend handles
- `app_.ActiveTickets`, `app_.activeTicketsMutex_`, `app_.activeTicketsPublished_`, `app_.ActiveTicketsRevision` — published-tickets state
- `app_.LastTrackerTicketSyncWarning`, `app_.lastTrackerConnectivityState_`, `app_.nextTrackerConnectivityProbeAt_` — connectivity probe state
- `app_.pendingLuaWindowBump_` — Lua coalesced-bump flag
- `app_.PushOfflineReplayTimersDuringTransportOutage()`, `app_.requestDeferredLiveTrackerBackendSuccessNotify_()`, `app_.NotifyLuaTicketDataChanged()`, `app_.WarmIssueTypeEditMetaAtStartAsync()`, `app_.PruneEditMetaCacheToActiveTickets()` — coordinator callbacks

Interface bundle:

```cpp
class ITicketSyncHost {
  public:
    virtual ~ITicketSyncHost() = default;
    virtual LocalCacheManager*  Cache() = 0;
    virtual ITrackerClient*     Backend() = 0;
    virtual ITrackerBackendFactory& BackendFactory() = 0;
    // Published tickets — handed back as a write-guard so the service doesn't
    // need raw access to the mutex.
    virtual std::unique_lock<std::mutex> LockActiveTickets() = 0;
    virtual std::vector<CachedTicket>& ActiveTickets() = 0;
    virtual void PublishActiveTicketsSnapshot() = 0; // bumps revision + publishes shared_ptr
    // Coordinator callbacks
    virtual void OnConnectivityDown() = 0;            // sets lastTrackerConnectivityState_ + nextProbeAt + PushOfflineReplayTimers
    virtual void OnSyncSucceededDeferred() = 0;       // calls requestDeferredLiveTrackerBackendSuccessNotify_
    virtual void OnTicketDataChanged() = 0;           // NotifyLuaTicketDataChanged
    virtual void OnFullSyncSuccess() = 0;             // WarmIssueTypeEditMetaAtStartAsync + PruneEditMetaCacheToActiveTickets
    virtual std::string&  SyncWarningSlot() = 0;      // LastTrackerTicketSyncWarning
    virtual void SetPendingLuaWindowBump() = 0;       // sets pendingLuaWindowBump_ true
};
```

AppController implements `ITicketSyncHost`. `TicketSyncService` ctor takes `ITicketSyncHost&`. Drop `friend class TicketSyncService;`.

Use `offline-sync` agent (it owns sync surfaces too) — verify by scope.

#### B3 · LuaAutomationHost — finish Phase 1B / 1C / 1D first, then drop friend

`LuaAutomationHost` is the longest piece of Track B because Phases 1B-1D haven't shipped. Recommended sub-slicing:

- **B3a — Phase 1B**: move `InitLua`, `InitLuaCore`, `InitLuaUi` (and the sol2 `lua` state member) from AppController into LuaAutomationHost. ~250 LOC migration. Use `lua-binder` agent.
- **B3b — Phase 1C**: move the ~18 `Lua*Bind` methods (+ associated members like `fieldDisplayCachedProviders_`, `luaFieldCache_`, `luaWindows_`, `pendingLuaWindowOps_`, the McpTools / TicketActions / GlobalActions registries, the FieldIconMaps). ~1100 LOC migration. The free-function glue stubs in `AppController_LuaBindings.cpp` move alongside their owners or stay as anon-namespace inside the new TU. Use `lua-binder` agent.
- **B3c — Phase 1D**: move the automation worker thread + job queue (`automationWorker_`, `automationJobs_`, `automationJobMutex_`, `automationWorkerShuttingDown_`, `AutomationWorkerLoop`, `RunAutomationJob`). Use `lua-binder` agent.
- **B3d — Phase 2**: now that LuaAutomationHost owns everything, define `ITrackerActions` interface for the small remaining surface (mostly `CreateIssue`, ticket-data reads). LuaAutomationHost ctor takes `ITrackerActions&`. Drop `friend class LuaAutomationHost;`.

After B3a-c, `AppController_LuaBindings.cpp` shrinks to a thin stub (or disappears entirely). The `AppController_LuaStubs.cpp` ↔ `AppController_LuaBindings.cpp` symmetry must be preserved per project rule (Optional plugins → keep in sync) — `LuaAutomationHost.cpp` ↔ `LuaAutomationHost_Stubs.cpp` becomes the new pair.

---

## Critical files

- `Source_Core/include/AppController.h` (friend declarations L94 / L99 / L104; Lua types L788–887)
- `Source_Core/src/Commands/BuiltinCommands.cpp` (split anchor lines 186 / 334 / 360 / 377 / 419 / 547 / 578 / 667 / 814 / 875 / 953 / 1641 / 1698 / 1305)
- `Source_Core/src/PlaneClient.cpp` (split anchors at the existing method headers L131 / L364 / L385 / L776 / L829 / L1144 / L1154 / L1356)
- `Source_Core/src/ConfigManager.cpp` (section L659 `ConfigManager methods`; helpers above)
- `Source_Core/src/SmatchetUI.cpp` (`Draw` L397; `drawMainMenuBar` L975 → 1423; `drawEnsureCatalogAndInitialSync` L886; layout helpers L286–365)
- `Source_Core/src/OfflineQueueService.cpp` (consumer of friend → swaps to `ICacheAccess&`)
- `Source_Core/src/TicketSyncService.cpp` (consumer of friend → swaps to `ITicketSyncHost&`)
- `Source_Core/include/OfflineQueueService.h` / `TicketSyncService.h` / `LuaAutomationHost.h` (replace `AppController&` ctor arg with interface)
- `Source_Core/src/AppController_LuaBindings.cpp` — receives Phases 1B/1C/1D migration; not split directly
- `docs/plans/INDEX.md` — add new entry indexing this plan + appendix to `docs/plans/shipped/` once shipped
- Stale references to remove during fix-up: `BACKLOG_CODE_REVIEW.md §1.7 / §7 item N` in `AppController.h` L92/98/102 + `OfflineQueueService.h` L4/L16 + `TicketSyncService.h` L5 + `LuaAutomationHost.h` L4 + `BuiltinCommands.cpp:332`

### Existing utilities to reuse (do not re-create)

- `MakeCommand`, `PString`, `PInt`, `PaginateJsonArray`, `PaginateString` — currently in anon namespace of `BuiltinCommands.cpp`; lift into `BuiltinCommands_Internal.h` for sub-files to share.
- `RegisterViewToggleCommands(reg, app)` (already a sibling file) — pattern blueprint for A1.
- `JiraIssueSearch.cpp` / `JiraIssueMutation.cpp` / `JiraUserAndMeta.cpp` — blueprint for A2.
- `BlameAnalysisUi_{Window,Modals,Launch,Preferences,Worker,Config}.cpp` + `BlameAnalysisUi_Internal.h` — blueprint for the SmatchetUI split shape (A4), and for the include-replication rule per AGENTS.md § Post-split include-replication rule.
- `MainThreadDispatcher` — existing worker→UI hand-off; reuse in any new interface implementation that needs to bounce a callback back to the UI thread (not expected to grow).

---

## Slice ordering (recommended)

Each slice is one PR. Ship low-risk Track A first to thin files; then Track B once review surface is small.

1. **PR A3** — ConfigManager split (no public surface change, easiest review)
2. **PR A2** — PlaneClient split (mirrors JiraClient precedent — reviewers know the shape)
3. **PR A1** — BuiltinCommands split (mechanical, but largest new-file count)
4. **PR A4** — SmatchetUI split
5. **PR A5** — AppController.h Lua types extraction
6. **PR B1** — OfflineQueueService → `ICacheAccess` (smallest friend surface, lowest risk)
7. **PR B2** — TicketSyncService → `ITicketSyncHost` (largest interface, but isolated)
8. **PR B3a–d** — LuaAutomationHost Phase 1B / 1C / 1D / 2 (four PRs; coordinated with `AppController_LuaStubs.cpp`)
9. **Fix-up PR** — remove dead `BACKLOG_CODE_REVIEW.md` references throughout, add `docs/plans/shipped/large-files-and-phase-2.md` index entry to `BACKLOG_PLANS.md`

Track A PRs are independent — can land in any order, in parallel, or by separate agents.

Track B PRs depend on Track A only insofar as `AppController_LuaBindings.cpp` line numbers shift; otherwise independent.

---

## Verification

Per-slice gates:

- `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — dual-target build green
- `cmake --build --preset ninja-test-msys2` — doctest rig (sanitizer build) green
- `bash scripts/dev/test-all.sh` — full automated suite
- After every Track A PR: confirm `wc -l` of the touched source files all sit ≤ ~800 LOC (target; 1000 LOC is the soft ceiling).
- After every Track B PR: `grep -n "friend class" Source_Core/include/AppController.h` shows the corresponding line gone.
- After PR B3d: zero `friend class` lines remain in `AppController.h`.

Smoke scenarios for Track B (run via `scenario.run`):

- `offline-replay` — exercises `OfflineQueueService` against `ICacheAccess`
- `streaming-sync-cancel` — exercises `TicketSyncService` supersede / cancel FSM
- `lua-automation-host` — exercises `LuaAutomationHost` init + binding + worker

Final regression gate (end-of-effort): `git-janitor` runs the unified test runner + dual-target build before merging the last PR.

### Plan revision after implementation (AGENTS.md mandate)

This plan file gets moved to `docs/plans/shipped/large-files-and-phase-2.md` after the last PR lands. Implementation log + Deviations from plan + Verification sections appended per the project rule. The stale `BACKLOG_CODE_REVIEW.md` references in source comments are replaced with a single pointer to the applied plan.

---

## Implementation log

Track A — mechanical file splits — fully shipped as of this revision:

- `caa8c6f` · **A5** · extract Lua recorder types out of `AppController.h` into `AppController_LuaTypes.h` under `namespace smatchet::lua`; `using` aliases preserve `AppController::ImCmd` / `LuaFieldCacheEntry` / `LuaWindowEntry` / `PendingLuaWindowOp` at 36 call sites. `AppController.h` 1035 → 992 LOC. Folded into the plan-doc PR ([#93](https://github.com/alexandrosk0/Smatchet/pull/93)) at the user's request to keep PR noise down.
- `4e7e0d4` · **A3** · split `ConfigManager.cpp` (1773 LOC) into `ConfigManager.cpp` (825) + `ConfigManager_PathUtils.cpp` (735) + `ConfigManager_Views.cpp` (313) + `ConfigManager_Internal.h` (78). Shared helpers live in `smatchet::config_detail::`. `tests/CMakeLists.txt` patched to compile the two new sibling `.cpp` (CommentTemplate ADL serializers + `config_detail` singletons live there). PR [#97](https://github.com/alexandrosk0/Smatchet/pull/97).
- `3c50fb2` · **A2** · split `PlaneClient.cpp` (1646 LOC) into `PlaneClient.cpp` (151) + `PlaneIssueSearch.cpp` (659) + `PlaneIssueMutation.cpp` (390) + `PlaneFieldCatalog.cpp` (510) + `PlaneClient_Internal.h` (38). Shared helpers in `smatchet::plane_detail::`. Mirrors the existing `JiraIssueSearch.cpp` / `JiraIssueMutation.cpp` / `JiraUserAndMeta.cpp` precedent one floor up. PR [#98](https://github.com/alexandrosk0/Smatchet/pull/98).
- `18abf80` · **A1** · split `Commands/BuiltinCommands.cpp` (1898 LOC) into a 53-LOC dispatcher + 17 per-category files under `Source_Core/src/Commands/Builtin/` (one `Register<Cat>Commands(reg, app)` each). Helpers in `smatchet::cmd::builtin_detail::`. `commands.list` count: 75 pre = 75 post (catalog byte-equivalent). PR [#100](https://github.com/alexandrosk0/Smatchet/pull/100).
- `fab0fc7` · **A4** · split `SmatchetUI.cpp` (1454 LOC) into `SmatchetUI.cpp` (821) + `SmatchetUI_MainMenu.cpp` (479) + `SmatchetUI_Layout.cpp` (264) + `SmatchetUI_Internal.h` (38). Lua-style helpers in `smatchet::ui_detail::`. PR [#101](https://github.com/alexandrosk0/Smatchet/pull/101).

Track B — interface-bundle / friend-class removal — **shipped via a different shape than originally planned**:

- **B1** (`OfflineQueueService` friend drop) — shipped via PR [#127](https://github.com/alexandrosk0/Smatchet/pull/127) (`b5fc194`) as `IOfflineQueueDeps` interface (`Deps` suffix, not `ICacheAccess`). Behaviour equivalent; naming deviation only.
- **B2** (`TicketSyncService` friend drop) — shipped via PR [#127](https://github.com/alexandrosk0/Smatchet/pull/127) (`b5fc194`) as `ITicketSyncDeps` interface (`Deps` suffix, not `ITicketSyncHost`). Behaviour equivalent; naming deviation only. (B1 + B2 landed together in the single PR.)
- **B3** (`LuaAutomationHost` friend drop) — **done via dead-code drop**, NOT the originally-planned Phase 1B/1C/1D ownership migration. PR [#144](https://github.com/alexandrosk0/Smatchet/pull/144) (`7e6762d`) shipped the `ILuaBindingHost` interface + TU lift of `InitLuaCore` + 11 sol2 glues — AppController stays the owner of binding methods (now expressed through `ILuaBindingHost` virtuals); only the TU boundary moved so tests can link binding code without ImGui. That made the original Phase 1B/1C/1D ownership-migration plan obsolete: the friend declaration on `LuaAutomationHost` was reserved for that migration and is now vestigial. Architect re-scope dropped it directly via `docs/plans/shipped/lua-host-friend-drop.md` — see that plan and this PR's friend-drop slice.

## B3 — done-via-dead-code-drop (see lua-host-friend-drop.md)

`docs/plans/shipped/lua-host-friend-drop.md` carries the full re-scope. Summary:

- `LuaAutomationHost::app_` field + ctor parameter were dead code (no `app_.…` access inside `LuaAutomationHost.cpp`).
- Friend declaration on `AppController` was vestigial — reserved for the obsolete Phase 1B/1C/1D migration.
- Single-slice drop: remove the friend, remove the dead field + forward-decl + `AppController.h` include, switch ctor to `= default`, update the one ctor call site (`AppController.cpp:1062`) to drop the `*this` argument. Originating PR + Implementation log in `lua-host-friend-drop.md`.

## Deviations from plan

- **A4 final size: 821 LOC, above the ≤ 700 LOC target.** `Draw` itself stays in the root TU and is large; further chunking would have meant splitting `Draw` into helper methods that aren't a natural shelving boundary. Stopped at the MainMenu + Layout split. Acceptable given the file went from 1454 → 821; the public surface didn't grow.
- **A4 `SmatchetUI_Internal.h` is NOT included from `SmatchetUI.cpp` or `SmatchetUI_MainMenu.cpp`.** The internal header does `#include "imgui.h"` then `#define ImGui SmatchetLocalizedImGui`, which clashes with `imgui_internal.h` consumers (the macro rewrites function names that `imgui_internal.h` then can't resolve). Workaround: those two TUs declare the `smatchet::ui_detail::` helpers via a local forward decl block at the top of the `.cpp` instead of including the header. Layout.cpp does include the header because it does not pull `imgui_internal.h`.
- **A1 ended up at 14 per-category files (plan said 13).** `BuiltinCommands_Tickets.cpp` (read commands) and `BuiltinCommands_TicketMutations.cpp` (write commands) split apart because the in-original-file `// === tickets ===` and `// === ticket (mutations) ===` dividers were already separate buckets; merging them would have lost the read/write distinction.
- **A6 deferred entirely.** `AppController_LuaBindings.cpp` (2648 LOC) untouched in Track A — it is destined to dissolve into `LuaAutomationHost.cpp` during Track B / Phase 1B-1D. Splitting it mechanically now would create files that get re-deleted under B3. Original plan already called this out; recording here for completeness.
- **Track B B1 + B2 landed together in PR #127 with `Deps` suffix, not `Access`/`Host`.** Interface names diverged from the plan (`IOfflineQueueDeps` vs `ICacheAccess`; `ITicketSyncDeps` vs `ITicketSyncHost`). Naming deviation only; the dependency-injection shape and behaviour are equivalent.
- **Track B B3 ownership migration superseded by PR #144's TU lift direction.** PR #144 shipped `ILuaBindingHost` and lifted `InitLuaCore` + 11 sol2 glues out into `AppController_LuaBindingsCore.cpp` — but AppController stayed the owner of every binding (it now implements `ILuaBindingHost`). The four-phase Phase 1B/1C/1D/2 migration into LuaAutomationHost would have re-litigated that ownership decision; instead the residual friend declaration was dropped as dead code via `docs/plans/shipped/lua-host-friend-drop.md`.

## Verification

- **A5** ([#93](https://github.com/alexandrosk0/Smatchet/pull/93)) — Standalone (362/362) ✓ · DX12 (109/109) ✓ · doctest (1/1) ✓ · test-all (61 assertions, 0 fails) ✓ · clang-format clean.
- **A3** ([#97](https://github.com/alexandrosk0/Smatchet/pull/97)) — Standalone (58/58) ✓ · DX12 (50/50) ✓ · doctest (1/1) ✓ · test-all (61 assertions, 0 fails) ✓ · clang-format clean.
- **A2** ([#98](https://github.com/alexandrosk0/Smatchet/pull/98)) — Standalone ✓ · DX12 ✓ · doctest (1/1) ✓ · test-all (61 assertions, 0 fails) ✓ · clang-format clean.
- **A1** ([#100](https://github.com/alexandrosk0/Smatchet/pull/100)) — Standalone ✓ · DX12 ✓ · doctest (1/1) ✓ · test-all (61 assertions, 0 fails) ✓ · clang-format clean. **Bonus**: `commands.list` count 75 pre = 75 post (catalog byte-equivalent before/after).
- **A4** ([#101](https://github.com/alexandrosk0/Smatchet/pull/101)) — Standalone ✓ · DX12 ✓ · doctest (1/1) ✓ · test-all (61 assertions, 0 fails) ✓ · clang-format clean.

Across all Track A PRs the two `ninja-ui-test-msys2` missing-binary failures in `scripts/dev/test-all.sh` were ignored — they are a pre-existing-state issue unrelated to the splits (the UI-test build preset is not configured locally in this worktree).

Track B verification (closed via lua-host-friend-drop slice):

- **B3 friend-drop slice** (this PR) — see `docs/plans/shipped/lua-host-friend-drop.md` § Verification for the full bucket-classified gate results. Closure rule (`grep app_ Source_Core/{include,src}/LuaAutomationHost.{h,cpp}` empty + `grep 'friend class LuaAutomationHost' Source_Core/include/AppController.h` empty) passed.

## Self-improvement signals captured

Three durable signals surfaced during Track A — worth banking for follow-up review or the Track B fix-up PR:

- **`(std::min) / (std::max)` Windows-macro convention** (tracker-backend agent, A2). The codebase uses the parenthesised form to defeat the `<windows.h>` `min`/`max` macros (no global `NOMINMAX`). Naïve `std::min` triggers the lint hook. Convention is in place at `JiraIssueSearch.cpp:469`. Worth adding to `AGENTS.md` § Quality.
- **cppcheck `useStlAlgorithm` false-positives on lift-and-shift** (command-system agent, A1). `push_back`-into-`nlohmann::json::array()` patterns trigger the rule even though the source / dest types differ. Cost ~5 round-trips on A1 alone. Two options for fix-up: (a) suppress the rule repo-wide (it's style, not correctness), or (b) document in `agents/command-system.md` that lift-and-shift edits will hit this.
- **ImGui macro-order trap when introducing a shared header** (orchestrator, A4). Any new internal header that does `#define ImGui SmatchetLocalizedImGui` after `#include "imgui.h"` cannot be included by TUs that also pull `imgui_internal.h` — the macro rewrites function names that the internal header can't resolve. Workaround: include the internal header only in TUs that don't touch `imgui_internal.h`, OR have callers forward-declare the helpers locally. Cost ~3 round-trips on A4.
