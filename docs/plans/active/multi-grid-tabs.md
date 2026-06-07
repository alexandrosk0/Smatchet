# Plan — Multiple grid panes (dockable; same or different tracker backend, side-by-side)

> **Slug**: `multi-grid-tabs` (matches this file's basename without `.md`).
>
> **Status**: `active` — driving in-flight work. Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s active → shipped (see § Archive).
>
> **Usage**: every section filled; non-applicable sections carry `N/A — <reason>`, never deleted.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Today Smatchet shows exactly one grid bound to one globally-selected tracker backend (`cfg.TrackerType` → one `ITrackerBackend` → one ticket cache → one active view). Switching backend (Preferences) tears down and rebuilds the single grid. Users who track work across two projects, two query-views, or two trackers (e.g. a Jira board + a GitHub repo) cannot see them at once.

This plan adds **grid panes**: multiple grids, each a **dockable window** bound to its own `(backend, saved-view)` pair (same backend or different). Because they are dockable, ImGui's dockspace gives both layouts natively — **stack as tabs** OR **drag to split side-by-side**. Prompted by user request (2026-06-06). Gated on a deliberate "test-first" decision: the layer this rewrites hardest (view-store / per-backend buckets / `Views` class / backend-swap) has **zero test coverage** today, so a regression net (Slice 0) lands before any production refactor.

**Locked decisions** (user, 2026-06-06):
1. **Layout = dockable grid windows** — each grid is its own ImGui window in the existing dockspace; the user stacks them as tabs or splits them side-by-side. No bespoke tab-bar or fixed splitter.
2. **Concurrency = visibility-scoped.** Every **visible** pane is **live + concurrent**: its own backend instance, its own sync worker, its own ticket cache, independently editable. A **hidden** stacked tab (docked behind another, not rendered this frame) is **cached/lazy**: sync paused, snapshot retained, refreshed when it next becomes visible. Concurrency is therefore bounded by the *visible* pane count (typically 2–4), not the total open count.
3. **Tab = open instance of a saved view** — pane identity is `(backendKey, viewId)`; `ViewDefinition` / `Views` reused unchanged.
4. **Cache-namespacing + offline-queue `BackendKey` are front-loaded** into the foundation (Slice 1) — concurrent live backends cannot share the flat `tickets(id)` table or an unattributed write queue. No longer deferrable.
5. **Foundation lands before Linear finishes** — the per-pane backend foundation (Slice 1) precedes [`linear-tracker-backend.md`](linear-tracker-backend.md), so Linear slots in as "just another backend a pane points at."
6. **Test-first** — Slice 0 (regression net) ships before Slice 1.

Intended outcome: after this lands, a user can open several dockable grid panes across the same or different tracker backends, **see them side-by-side**, and each visible pane syncs and edits independently.

> **Supersedes the earlier lightweight framing.** An initial revision of this plan assumed one live backend + one sync worker with only the active tab syncing. Decision 2 (all visible panes live) replaces that: AppController de-singletons into N concurrent grid contexts, multiple sync workers run at once, and cache/queue concurrency safety moves to the foundation.

## Approach

Split the singleton into two units:

- **`GridPane`** — the persistent, dockable-window unit: identity `{id, title, backendKey, viewId}` + UI runtime `{gridState, sort/filter cache, focused flag}`. Persisted in `smatchet_panes.json`; dock geometry rides ImGui's existing `.ini`.
- **`GridLiveContext`** — the live engine bundle attached to a pane **while it is visible**: its own `ITrackerBackend`, its own `TicketSyncService`, its own in-memory ticket cache + published `shared_ptr` snapshot, and its resolved field-catalog key.

`AppController` becomes a manager of `map<paneId, GridLiveContext>` for the visible set (was one backend + one sync + one `ActiveTickets`). A pane becoming visible spins up its context (construct backend, start sync); a pane going hidden pauses its sync and **retains the cached snapshot** (the context may be retired to the existing defer-free graveyard after a grace window). Grid render is made **re-entrant** — the draw function takes a `GridPane&` + its snapshot and runs once per visible window per frame.

Concurrency safety is front-loaded because two live backends now write concurrently: the SQLite `tickets`/`ticket_field_values` tables gain a `backend_key` (+ project) scope; `PendingCreate`/`PendingFieldEditRecord` gain `BackendKey` and replay against the matching backend; sync completions are tagged with `paneId` and routed through `MainThreadDispatcher` to the right pane (the old "apply to the active tab" assumption breaks when several are live). A single **focused pane** still drives global actions (command palette, new-issue draft, toolbar, keyboard nav, AI auto-context).

The trade-off accepted: N concurrent sync workers + N grids rendering per frame cost more CPU/memory than the rejected single-live model, bounded by visible-pane count and mitigated by per-pane virtualization + a shared dispatcher-drain budget. Sequencing is **test-first then foundation-first**: Slice 0 → S1 (de-singleton + concurrency-safe storage, the Linear prerequisite) → S2 (dockable windows + re-entrant render) → S3 (concurrent live contexts, visibility lifecycle, dispatcher routing, focus model) → S4 (cross-cutting commands/MCP/Lua/AI/offline-concurrent-replay) → S5 (durable cache + memory cap + perf + thread-sanitizer). S3–S5 may interleave with Linear; only S1 blocks it.

### Slice 0 — Regression net (test-first, ships before any production change)

All **bucket-A** (deterministic; the ImGui-Test-Engine bucket-E lane is harness-wide flaky per [`b8-bucket-e-coverage.md`](b8-bucket-e-coverage.md) § Deviations, so the net avoids it). All **characterize-existing** (pin current single-grid behaviour so the refactor's breakage is loud). Two workstreams:

**Workstream 1 — multi-grid seam net** (`test-rig`):
- `tests/Core/ConfigManagerViews.test.cpp` (new): `NormalizeViewsBackendKey`; `LoadViewsOrBootstrap` per-backend default field-sets; v2 `backends` round-trip; **multi-bucket coexistence** (writing the Plane bucket must not clobber Jira's — the core multi-grid invariant); legacy v1→Jira migration; `EnsureViewBucketBootstrapped` idempotent.
- `tests/Core/ViewsClass.test.cpp` (new): `EnsureLoaded` slice select; `Activate`/`Create`/`UpdateActive`/`DeleteActive`; `GetRevision()` monotonic + the `BumpRevision` cache-key contract.
- `tests/Core/TicketSyncService.test.cpp` (extend): swap matrix (Jira→Plane→GitHub create-the-right-client via `ScriptedTrackerBackendFactory`); same-kind = no swap; kind-change clears active tickets; supersede/defer (`SyncWithBackend` while busy → `Cancelled`/`Superseded`/`pendingConfig_`).

**Workstream 2 — backend deepening** (`test-author` + `tracker-backend` consult):
- catalog-build fixture (new, `tests/support/` + `tests/Core/TrackerCatalogBuild.test.cpp`): drives the **real** `FetchFieldCatalog` / `MergeProjectComponentsFromEndpoint` against scripted HTTP (cpp-httplib, already linked) — closes the tracked `test.md` P2 (`JiraFakeTrackerFixture` injects a pre-built catalog, bypassing build, so the components-rendered-as-text bug class is fixture-invisible). Asserts: unscoped fetch leaves `components` dropdown-eligible, scoped fetch populates per-project options, `customfield_*` arrays classify per schema.
- `tests/Core/{Plane,Jira,GitHub}IssueMappingPure.test.cpp` (extend, `test-rig`): null / missing-relation / empty-optional mapping edges per backend.

Slice 0 ships as PR A (Workstream 1) + PR B (Workstream 2), independent → parallelizable.

## Files to modify

**Slice 0 — tests (no production change):**
1. `tests/Core/ConfigManagerViews.test.cpp` (new) — over [`ConfigManager_Views.cpp:224`](../../../Source/Core/src/Config/ConfigManager_Views.cpp:224) / [`:323`](../../../Source/Core/src/Config/ConfigManager_Views.cpp:323) / [`:140`](../../../Source/Core/src/Config/ConfigManager_Views.cpp:140) / [`:301`](../../../Source/Core/src/Config/ConfigManager_Views.cpp:301). Grep-confirmed absent.
2. `tests/Core/ViewsClass.test.cpp` (new) — [`Views`](../../../Source/Core/include/Ui/Views.h:11). Grep-confirmed absent.
3. `tests/Core/TicketSyncService.test.cpp` (extend) — [`SwapBackendIfTrackerChanged`](../../../Source/Core/src/Sync/TicketSyncService.cpp:473) + [busy-defer](../../../Source/Core/src/Sync/TicketSyncService.cpp:459).
4. `tests/support/<CatalogBuildFixture>.h` (new) + `tests/Core/TrackerCatalogBuild.test.cpp` (new). Grep `rg -l 'FetchFieldCatalog' tests/` first.
5. `tests/Core/{Plane,Jira,GitHub}IssueMappingPure.test.cpp` (extend).
6. `tests/CMakeLists.txt` — register new TUs.

**Slice 1 — de-singleton + concurrency-safe storage (the Linear prerequisite):**
7. `Source/Core/include/GridPane.h` (new) — `GridPane` (identity + UI runtime). Grep `rg -l 'GridPane' Source/Core/` first.
8. `Source/Core/include/GridLiveContext.h` (new) + impl — the per-visible-pane engine bundle (`unique_ptr<ITrackerBackend>` + `TicketSyncService` + ticket cache + published snapshot + catalog key). Extracted from today's singleton members in [`AppController.h:989`](../../../Source/Core/include/AppController.h:989) (`ticketSync_`) + [`:1000`](../../../Source/Core/include/AppController.h:1000) (`ActiveTickets`).
9. [`Source/Core/include/AppController.h`](../../../Source/Core/include/AppController.h:989) — own `map<paneId, unique_ptr<GridLiveContext>>` (start size 1, behaviour-identical); retire-to-graveyard ([`:960`](../../../Source/Core/include/AppController.h:960)) on context teardown. Sync completions route **structurally** — each context owns its own `TicketSyncService` + per-context deps adapter, so no `paneId` tagging is needed at the deps layer (`paneId` attribution is only for UI cues/toasts + the shared `TickAllContexts` budget, Slice 3). *Revised per the [slice1-design addendum](multi-grid-tabs-slice1-design.md) § 3.2 — the original "tagged with paneId, routed via MainThreadDispatcher" wording over-specified a channel that already exists structurally.*
10. [`Source/Core/include/Ui/SmatchetUiSession.h:139`](../../../Source/Core/include/Ui/SmatchetUiSession.h:139) — `UiDrawSession` holds `std::vector<GridPane>` + `focusedPaneId`; migrate per-pane UI fields (`gridState` [`:424`](../../../Source/Core/include/Ui/SmatchetUiSession.h:424), sort/filter caches `cachedSortedIndices`/`filteredIndices`/`cachedSort*Revision` — **per-pane so a re-render is an O(1) cache hit**, see § Performance, `lastViewsBackendKey` [`:293`](../../../Source/Core/include/Ui/SmatchetUiSession.h:293)) into `GridPane`.
11. [`Source/Core/src/Persistence/LocalCacheManager.cpp:104`](../../../Source/Core/src/Persistence/LocalCacheManager.cpp:104) — namespace `tickets` + `ticket_field_values` by `backend_key` (+ project); schema migration. **Front-loaded** (concurrent live backends share the DB).
12. [`Source/Core/include/CachedTicketTypes.h:51`](../../../Source/Core/include/CachedTicketTypes.h:51) (`PendingCreate`) + [`:63`](../../../Source/Core/include/CachedTicketTypes.h:63) (`PendingFieldEditRecord`) — add `BackendKey`; + DB columns + migration in [`LocalCacheManager.cpp:28`](../../../Source/Core/src/Persistence/LocalCacheManager.cpp:28)/[`:118`](../../../Source/Core/src/Persistence/LocalCacheManager.cpp:118); replay matches the record's backend (Sync subsystem — every-write-through-queue invariant). **Front-loaded.**
13. `Source/Core/include/Config/ConfigManager.h` + `Source/Core/src/Config/ConfigManager_Panes.cpp` (new) — `smatchet_panes.json`: ordered `[{id,title,backendKey,viewId}]` + `focusedPaneId`; default-bootstrap one pane from `cfg.TrackerType` + its active view (zero-migration); atomic-write + `ScopedFileLock` mirroring the views file.

**Slice 2 — dockable grid windows + re-entrant render:**
14. `Source/Core/src/Ui/SmatchetActiveProjectGridUi*.cpp` — make the grid-draw fn **re-entrant**: takes `GridPane&` + its snapshot, renders once per visible window per frame (today it reads `g_ui.gridState` + the single active view directly). Per-pane `ImGuiListClipper` virtualization.
15. `Source/Core/src/Ui/SmatchetGridPaneWindows.{h,cpp}` (new) — one dockable ImGui window per `GridPane` (Begin/End per pane; title = backend icon + view name; `+` new pane, X close). Dockspace already in use ([`SmatchetViewVisibility.h`](../../../Source/Core/include/Ui/SmatchetViewVisibility.h)) — stacking/splitting is native ImGui docking, no bespoke layout.
16. [`Source/Core/src/Sync/TicketSyncService.cpp:473`](../../../Source/Core/src/Sync/TicketSyncService.cpp:473) — per-context swap on pane open; [`Views`](../../../Source/Core/include/Ui/Views.h:21) `Activate(pane.viewId)` per pane.

**Slice 3 — concurrent live contexts + visibility lifecycle + focus:**
17. `AppController` + `GridLiveContext` — visibility-driven lifecycle: visible → ensure context + start sync; hidden → pause sync, retain snapshot, retire after grace; per-context cancel/`Superseded`; N concurrent sync workers, all off-UI-thread. **PLUS the deferred field-catalog move** (added per [slice1-design addendum](multi-grid-tabs-slice1-design.md) § 3.1): the in-memory catalog block (`TrackerFieldCatalogRevision`/`AvailableFields`/`AvailableComponents`/`AvailableIssueTypeMeta`/`currentCatalogProjectKey_`/`projectComponentOptions_`, [`AppController.h:1014–:1046`](../../../Source/Core/include/AppController.h:1014)) is mutex-guarded but **semantically single-backend** — it MUST move per-context here, before two contexts go live, or different-backend panes overwrite each other's catalog.
18. [`MainThreadDispatcher`](../../../Source/Core/include/MainThreadDispatcher.h:32) consumers — sync apply tagged by `paneId`; the per-frame drain budget split/round-robined across panes so N simultaneous applies can't blow one frame (see § Performance).
19. `UiDrawSession` + grid UI — focused-pane model: global actions (command palette, new-issue draft, toolbar, keyboard) target `focusedPaneId`.

**Slice 4 — cross-cutting:**
20. `Source/Core/src/Commands/{BuiltinCommands,ViewCommands}.cpp` — `pane.new/close/next/prev/rename/duplicate/split`; re-point `view.*`, Command Palette, MCP tool schemas, Lua bindings, AI auto-context at the focused pane; per-backend `ToolbarAppend` follows focus; offline-queue concurrent replay (BackendKey-matched, landed S1) exercised with two live backends queuing at once.

**Slice 5 — durable cache + memory cap + perf:**
21. `LocalCacheManager` — persist pane snapshots across restart (the S1 namespacing makes this natural); LRU memory cap on **hidden** cached panes only (visible never evicted); perf scenarios + thread-sanitizer pass.

## Existing utilities reused

- `AppController::GetActiveTicketsSnapshot()` — [`AppController.h:600`](../../../Source/Core/include/AppController.h:600): `shared_ptr<const vector<CachedTicket>>`; each pane holds its own cheaply; same-`(backend,view)` panes can share the pointer.
- `TicketSyncService::SyncWithBackend(configOverride, viewsOverride)` — [`TicketSyncService.cpp:450`](../../../Source/Core/src/Sync/TicketSyncService.cpp:450): already drives a sync against an arbitrary `(config, views)` pair — each per-pane context calls it with its own pair.
- `TicketSyncService::SwapBackendIfTrackerChanged` + `Superseded`/`Cancelled` — [`TicketSyncService.cpp:473`](../../../Source/Core/src/Sync/TicketSyncService.cpp:473) / [`:459`](../../../Source/Core/src/Sync/TicketSyncService.cpp:459): per-context backend resolution + cancel; now one cancel FSM **per** context, not one global.
- Retired-backend graveyard (ADR-0012) — [`AppController.h:960`](../../../Source/Core/include/AppController.h:960): defer-free retire of a torn-down pane's backend.
- `MainThreadDispatcher` drain-time budget — [`MainThreadDispatcher.h:32`](../../../Source/Core/include/MainThreadDispatcher.h:32): the existing FIFO-until-budget drain is the seam for routing + budgeting N panes' sync applies.
- `ConfigManager` per-backend view buckets — [`ConfigManager_Views.cpp:323`](../../../Source/Core/src/Config/ConfigManager_Views.cpp:323): panes reference into the v2 `backends` map.
- `FieldCatalogCache` `backend|endpoint|project` keying — already multi-backend + concurrency-keyed; each context resolves its key ([`FieldCatalogCacheScopedRoundTrip.test.cpp`](../../../tests/Core/FieldCatalogCacheScopedRoundTrip.test.cpp)).
- `CacheEvictionPolicy::CacheOverCap` — [`CacheEvictionPolicy.h:17`](../../../Source/Core/include/CacheEvictionPolicy.h:17): LRU over hidden cached panes.
- `MemorySnapshot` / `perf.memory` — [`MemoryTelemetry.h:24`](../../../Source/Core/include/MemoryTelemetry.h:24): extend with per-pane gauges.
- Test seams: `tests/support/ScriptedTrackerBackendFactory.h`, `FakeTicketSyncDeps.h`, `SqliteMemFixture.h`, `Fake{Plane,GitHub}Fixture.h`, `JiraFakeTrackerFixture.h`.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: N visible grids now render per frame — each virtualized (`ImGuiListClipper`); the shared budget covers all visible panes; N sync workers run off the UI thread; the dispatcher drain budget is split across panes. Slice 0 is test-only. Full analysis → § Performance.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: every per-context sync runs on its own worker via `SyncWithBackend` — zero sync I/O on the UI thread; context spin-up on pane-show is async; each pane carries its own sync cue. Annotate context boundaries `/* PILLAR2_WORKER_ONLY */`.
- **Pillar 3 (never crash)**: N concurrent backends/workers + shared singletons (`LocalCacheManager`, `FieldCatalogCache`, `BackendAuditTrail`, `ConfigManager`, `MainThreadDispatcher`) demand audited thread-safety — TSan pass (Slice 5); a pane referencing a deleted view falls back to the backend default; torn-down backends retire via the graveyard (no use-after-free).
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: pane focus + switch must be keyboard-driveable (Ctrl+Tab across panes; `pane.next/prev`); titles honour font scaling. Bucket-E reachability backlogged (Pillar 4 aspirational); a bucket-C/E gap on the visible windows triggers the visual-validation pause at ship (`AGENTS.md` § Autonomous ship-loop default exception 5).

## Memory impact

Per-pane resident cost = ticket snapshot + grid runtime + sort/filter index vectors; the field catalog is **shared** (keyed `FieldCatalogCache`, not duplicated). **Visible** panes are live (snapshot + active sync buffers); **hidden** panes hold snapshot only (sync paused) and are LRU-evictable.

| Component | Structure | Approx cost |
|---|---|---|
| Ticket snapshot | `shared_ptr<const vector<CachedTicket>>` ([CachedTicketTypes.h:13](../../../Source/Core/include/CachedTicketTypes.h:13)) | **dominant**. Per ticket = `id` + `fieldValues` (map, ~8–15 entries) + `fieldRichValues` (rich fields only). Short-field ticket ≈ 1–2 KB; with a description / ADF rich value ≈ 5–20 KB |
| Grid runtime | `SpreadsheetState` ([SpreadsheetState.h:68](../../../Source/Core/include/SpreadsheetState.h:68)) | < 1 KB baseline (scalars + 512+128+128 char bufs + `set<int>` selections) |
| Sort/filter caches | `cachedSortedIndices` + `filteredIndices` | ~8·n bytes (one `size_t` per ticket) |
| Live sync buffers | per-context `TicketSyncService` pending-batch + keep-id sets | transient, freed per session |

Sizing: a 500-ticket view ≈ 0.5–2.5 MB/pane. Resident total scales with **visible** pane count (user-controlled split): 4 visible ≈ 2–10 MB live + hidden cached panes under the LRU cap. Bounded.

Controls:
- **Shared snapshots** — same-`(backend, view)` panes share the `shared_ptr`; only distinct views allocate.
- **LRU eviction (hidden only)** — `CacheEvictionPolicy::CacheOverCap(count, bytes, maxCount, maxBytes)` ([CacheEvictionPolicy.h:17](../../../Source/Core/include/CacheEvictionPolicy.h:17)) over **hidden** cached panes; cap by count AND aggregate bytes; evict least-recently-visible (drop snapshot → re-sync on re-show). **Visible panes are never evicted.**
- **Hidden = sync paused** — a hidden pane frees its live sync buffers + worker; only the snapshot stays.
- **Telemetry** — extend `MemorySnapshot` ([MemoryTelemetry.h:24](../../../Source/Core/include/MemoryTelemetry.h:24)) with `GridPaneCount`, `VisiblePaneCount`, `PaneSnapshotApproxBytes` (Σ resident snapshots) so the existing `perf.memory` gauge surfaces multi-grid pressure. Pull-based — no new hot-path call.

Measurement: actual per-ticket bytes + resident total validated via `perf.memory` against a 2-visible-pane × N-ticket fixture during Slice 3/5 — **not estimated at ship**.

## Performance

Steady-state budget **6.94 ms** (144 Hz); p99 ≤ 16.67 ms (60 Hz floor) per `AGENTS.md` § Quality Pillars. The headline change vs the single-grid app: **N visible grids render per frame** and **N sync workers run concurrently** — both must stay within the SHARED budget.

**Hidden panes cost zero per frame** — not rendered, not sorting, not syncing.

**Per-frame render (N visible panes share 6.94 ms):**
- Each pane's grid is virtualized (`ImGuiListClipper`) → render cost ∝ *visible rows*, not ticket count.
- Sort/filter caches are **per-pane** (`cachedSortedIndices`, `filteredIndices`, `cachedSort*Revision`, `cachedSortValid`) → a steady-state frame re-uses each pane's cache (O(1)); a sort recompute (O(n log n), zero-copy `GetFieldValueRef` comparator [`CachedTicketTypes.h:33`](../../../Source/Core/include/CachedTicketTypes.h:33)) fires only when that pane's ticket/catalog revision bumps. A **global** sort cache would thrash across panes — rejected.
- Risk: 3–4 grids drawing simultaneously is ~N× the single-grid draw. Mitigation: virtualization caps per-pane cost; the `tab-switch`/`side-by-side` perf scenario gates it; if N×draw exceeds budget, cap simultaneous visible panes or prioritize the focused pane.

**Concurrent sync (N workers):**
- Each visible pane's `TicketSyncService` runs its own worker — N× background CPU, all off the UI thread (Pillar 2 holds).
- **Shared-budget hazard**: every worker posts its apply through `MainThreadDispatcher`; the UI-thread apply budget is 3 ms / 20 tickets ([`TicketSyncService.h:113`](../../../Source/Core/include/Sync/TicketSyncService.h:113)) **per session** today — N panes applying at once could stack N× on one frame. Mitigation: the dispatcher drain-time budget ([`MainThreadDispatcher.h:32`](../../../Source/Core/include/MainThreadDispatcher.h:32)) already bounds *total* per-frame work and defers overflow to next frame; route per-pane applies through it and round-robin so no single frame exceeds budget (deferred-tasks gauge already exists).

**De-singleton overhead guard**: per-context lookup (`paneId → GridLiveContext`) must be O(1) and off the per-cell path; the render fn receives the resolved `GridPane&` once per window, not per cell.

Measurement: `side-by-side-2-grid` (two visible panes, assert frame time) + `concurrent-sync` (two panes syncing, assert UI-thread apply stays in budget) perf scenarios added Slice 3/5 + gated PR-fast (§ Perf-review-system gates).

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

Per `docs/plans/shipped/pillar-1-2-perf-review-system.md`.

- **Slice 0**: **N/A** — test-only; no `Source/Core/` runtime path, no hot path.
- **Slices 1–5** (touch `Source/Core/`):
1. **PR-fast CI** — **fires**. Existing `priority-grid-scroll` (per-pane render) + new `side-by-side-2-grid` + `concurrent-sync` scenarios; declare in `scripts/dev/perf-pr-fast-set.json`.
2. **Pillar 2 static scanner** — **fires**. Per-context sync + pane-show context spin-up must be worker-only; annotate boundaries `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms`. No new UI-thread sync I/O.
3. **Dispatcher drain** — **fires**. Slice 3 changes how per-pane applies are routed/budgeted through `MainThreadDispatcher::Drain()`; re-validate the drain budget under N concurrent producers.
4. **Visible-cue bucket-E harness** — **fires**. Each pane's sync stall shows its own cue; extend the bucket-E cue coverage for a concurrent-sync stall path.
5. **Marker inventory** — **fires conditionally**. If Slice 2/3 adds `SMATCHET_UI_PERF_SCOPE("pane.render"/"pane.switch")`, regen `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push local check**: `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against `priority-grid-scroll` + `side-by-side-2-grid` + `concurrent-sync`.

**Override**: `perf-out-of-band` PR label per `AGENTS.md` § Merge gates — intentional regression + baseline-bump PR queued only.

## Risks / non-goals

- **Concurrency / thread-safety (highest risk)** — N live backends + N sync workers hammer shared singletons (`LocalCacheManager`/SQLite, `FieldCatalogCache`, `BackendAuditTrail`, `ConfigManager`, `MainThreadDispatcher`). Mitigation: audit each for re-entrancy; namespaced DB writes (item 11/12) keep panes' rows disjoint; **ThreadSanitizer pass mandatory** (Slice 5); a data race here is a Pillar-3 CRITICAL.
- **Shared dispatcher budget blown by N applies** — N panes posting sync applies could stack on one UI frame. Mitigation: route through the existing drain-time budget + round-robin; gated by the `concurrent-sync` scenario. Full analysis → § Performance.
- **N× render cost** — 3–4 grids per frame. Mitigation: per-pane virtualization + a cap on simultaneous visible panes. Full analysis → § Performance.
- **Memory** — N live caches, none evictable while visible. Mitigation: hidden-only LRU + `perf.memory` gauge. Full analysis → § Memory impact.
- **De-singletoning AppController is wide** — backend/sync/cache members move into `GridLiveContext`; many call sites assume the singleton. Mitigation: Slice 1 lands a 1-context map (behaviour-identical); routable to `architect` (cross-cutting) + `grid-engine`/`mechanic` for the mechanical follow-through.
- **Non-goal: hidden tabs stay live** — hidden stacked tabs are cached/lazy (sync paused); concurrency is visible-scoped, not total. (Always-live-when-hidden explicitly rejected — unbounded.)
- **Non-goal: two instances of the same backend type** (two Jira servers — multi-profile credentials). `backendKey` is shaped to later become `profileId`; v1 same-type panes share one credential set.
- **Non-goal: cross-pane aggregation / joined view** — each pane is one `(backend, view)`.
- **Non-goal: tear-off into separate OS windows** — panes live in the single ImGui dockspace (docking gives split + stack; OS-level multi-window is out).

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps.

- **Bucket A (pure-logic ctest, `test-rig`)**: Slice 0 (`ConfigManagerViews`, `ViewsClass`, `TicketSyncService` swap+supersede, mapping edges); later — `GridPane`/`GridLiveContext` serialize/restore, default-pane bootstrap (zero-migration), **namespaced-cache round-trip** (two backends' rows disjoint), **offline-replay BackendKey match** (S1), per-pane sort-cache isolation, dispatcher round-robin budget (S3).
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: Slice 2+ dockable-pane new/close/focus + a split (side-by-side two panes) boot-open-assert smoke — authored against the warmup gate, run ~4× back-to-back (lane is known-flaky; a new flake is a real gate regression to file).
- **Bash-driver scenario / screenshot / sanitizer**: `side-by-side-2-grid` + `concurrent-sync` perf scenarios; **ASan** after S1 (DB/queue migrations touch user data) + **TSan** at S5 (concurrent backends — mandatory). No golden image expected.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) on every `Source/Core/`-touching slice (1–5); Slice 0 is test-only.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model (pane vs view vs backend vs profile vs dockspace-panel terminology; the `GridPane` ⇄ `GridLiveContext` ⇄ `ViewWorkspaceState` boundaries; "visible-live vs hidden-lazy" lifecycle) + sharpen terms before finalising; record the outcome. Required — do not delete.
- **Manual residue**: the side-by-side dock-layout visual check (Pillar 4) may remain manual until bucket-E covers a split — name the deferred-automation action + add a `docs/self-improvement/categories/tooling.md` entry. No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here (multi-profile, cross-pane aggregation, OS-window tear-off, always-live-hidden), and revise or delete them. **In particular**: update [`linear-tracker-backend.md`](linear-tracker-backend.md) — drop the "single active backend / single active team" non-goal wording (line ~129), which Slice 1 supersedes; Linear stays single-team-*per-pane* for MVP but multiple concurrent Linear panes across teams become possible.

- **Linear backend itself** — separate plan ([`linear-tracker-backend.md`](linear-tracker-backend.md)); this plan only provides the pane foundation it slots into.
- **Multi-profile (two same-type backends)** — follow-up; `backendKey`→`profileId` forward-compat noted.
- **Cross-pane aggregation / joined view** — follow-up; needs separate UX.
- **OS-level multi-window tear-off** — no-action; the ImGui dockspace covers split + stack.
- **Concurrency design depth** — the AppController→N-context redesign + shared-singleton thread-safety audit warrants an `architect` design pass (and possibly an ADR) before Slice 1 implementation; flagged, not designed here.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

- `e221f99c` · Slice 0 WS2 (PR B) — real Jira catalog-BUILD fixture (`tests/support/JiraCatalogHttpFixture.h` + `tests/Core/TrackerCatalogBuild.test.cpp`, 11 cases) + 22 null/missing-relation/empty-optional mapping edges across `{Jira,Plane,GitHub}IssueMappingPure.test.cpp`; closes the `test.md` P2 (2026-06-01) catalog-build blind spot. Test-only.
- `feat/mgt-s1a-context` · Slice 1a — `GridLiveContext` extraction (`Backend` / `ticketSync_` / `ActiveTickets` + published snapshot + revision + mutex moved verbatim out of AppController; new `GridLiveContext.{h,cpp}`); `AppControllerDepsAdapter` → per-context `GridContextDepsAdapter(AppController&, GridLiveContext&)` (the design-addendum § 3.2 chokepoint — `ITicketSyncDeps` and the Slice-0 net unmodified); AppController owns `map<int, unique_ptr<GridLiveContext>>` with a single `kDefaultPaneId` entry and public methods as `focusedContext()` delegators. Behaviour-identical; field-catalog block, `tickets_v2`, and queue `BackendKey` deferred to S3/1b/1c as designed (ADR-0018).
- `feat/mgt-s1b-tickets-v2` · Slice 1b — `tickets_v2` backend-key namespacing (design § 3.5, ADR-0018 decision 4): new `tickets_v2` / `ticket_field_values_v2` / `ticket_field_rich_values_v2` tables with `(backend_key, …)` PKs + cache_meta-gated one-time copy migration (`RunOneTimeTicketsV2CopyMigration`, legacy v1 tables retained, additive-only); all 7 `LocalCacheManager` ticket methods gain a `backendKey` first parameter; `GridLiveContext.backendKey` wired (mutex-guarded accessors) at `InitConfig`/`InitBackends` and re-stamped in `SwapBackendIfTrackerChanged` via new `CacheBackendKey`/`SetCacheBackendKey` on `ITicketSyncDeps` (+ read-only getter on `IOfflineQueueDeps` for replay's pipeline cache-seeding); `IssueCreatePipeline::Run` gains `cacheBackendKey`. Bucket-A: `tests/Core/LocalCacheTicketsV2Migration.test.cpp` (pre-migration fixture-DB round-trip, idempotence, empty-key guard, GitHub-vs-Plane `#123` disjointness). Pending-queue `BackendKey` + replay matching remain Slice 1c.

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/multi-grid-tabs.md docs/plans/shipped/` (move into the shipped tier),*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md`. Write new plan references tier-less.*
