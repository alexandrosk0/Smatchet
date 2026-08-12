# Design addendum — multi-grid-tabs Slice 1 (AppController de-singleton + concurrency foundation)
<!-- plan-date: 2026-06-06 -->

> **Status**: `shipped` — all cited PRs merged (see Implementation log); archived 2026-06-16 via plan-archival sweep.
>
> Companion to [`multi-grid-tabs.md`](multi-grid-tabs.md) § Out of scope item "Concurrency design
> depth" — the architect design pass that plan required before Slice 1 implementation
> (2026-06-06). ADR: [`docs/adr/0018-multi-grid-pane-contexts.md`](../../adr/0018-multi-grid-pane-contexts.md).

## 1. Goal

Define exactly which `AppController` members move into `GridLiveContext`, the chokepoint-based
migration strategy that keeps Slice 1 behaviour-identical and bisectable, the thread-safety
verdict for each shared singleton under N concurrent sync workers, and the SQLite `backend_key`
schema migration.

## 2. Affected components

| Component | Shape of change |
|---|---|
| `Source/Core/include/GridLiveContext.h` (new) + `src/GridLiveContext.cpp` | new file — per-pane engine bundle |
| `Source/Core/include/AppController.h` / `src/AppController.cpp` | interface-preserving: members :970–:1013 move out; public methods become delegators into the focused context |
| `Source/Core/src/AppControllerDepsAdapter.{h,cpp}` | **the chokepoint** — becomes `GridContextDepsAdapter(AppController&, GridLiveContext&)` |
| `Source/Core/src/Persistence/LocalCacheManager.{h,cpp}` | new `tickets_v2` family + `backend_key` columns on pending queues + one-time copy migration (strict zone, additive-only) |
| `Source/Core/include/CachedTicketTypes.h` | `BackendKey` member on `PendingCreate` / `PendingFieldEditRecord` (+ dead twins) |
| `Source/Core/src/Sync/OfflineQueueService.cpp` | replay-matching rule (backend-scoped) |
| `Source/Core/src/Config/ConfigManager_Panes.cpp` (new) | as planned (item 13) — unchanged by this addendum |
| NOT touched in Slice 1 | the ~22 external `GetActiveTicketsSnapshot()` call sites, MCP schemas, Lua bindings, grid UI — all keep compiling via delegators |

## 3. Interface contracts + design decisions

### 3.1 What moves into `GridLiveContext` (Slice 1)

From `AppController.h`, verbatim member moves:

| Member | Evidence | Note |
|---|---|---|
| `std::shared_ptr<ITrackerBackend> Backend` | `AppController.h:970` | keeps the ADR-0012 `atomic_load`/`atomic_store` discipline per-context |
| `std::unique_ptr<TicketSyncService> ticketSync_` | `:1000` | one sync FSM per context |
| `std::vector<CachedTicket> ActiveTickets` + `activeTicketsPublished_` + `ActiveTicketsRevision` | `:1011–:1013` | + the `activeTicketsMutex` that guards them |
| per-context deps adapter | `:989` | see 3.2 |
| `backendKey` / `catalogKey` (new strings) | — | namespacing + `FieldCatalogCache` key |

**What stays in AppController (shared)**: `Cache` (`:963`), `backendFactory_` (`:964`), the
retired-backend graveyard `retiredBackendsMutex_`/`retiredBackends_` (`:976–:977`) — **one
graveyard for all contexts**; `RetireBackend` (`:982`) is already thread-safe and per-context
adapters call it; `offlineQueue_` (`:994`, shared, BackendKey-routed); `luaHost_`,
`commandRegistry_`, dispatcher.

**Explicitly deferred to Slice 3 (flagged, not silent)**: the field-catalog block
`TrackerFieldCatalogRevision` / `AvailableFields` / `AvailableComponents` /
`AvailableIssueTypeMeta` / `currentCatalogProjectKey_` / `projectComponentOptions_`
(`AppController.h:1014–:1046`). These are mutex-guarded (`availableFieldsMutex_`, `:1015`) so
they're race-safe, but they are **semantically single-backend** — two live different-backend
panes would overwrite each other's catalog. Safe to defer only because Slices 1–2 never run two
live contexts. This is the largest hidden S3 work item; the plan's item 17 must absorb it
(plan revised in the same PR as this addendum).

Container: `std::map<int, std::unique_ptr<GridLiveContext>>` — `unique_ptr` because the context
holds atomics + mutexes (non-movable); `int paneId` with `kDefaultPaneId = 0`. C++14-clean (no
structured bindings in iteration; explicit `it->second`).

### 3.2 The chokepoint — `AppControllerDepsAdapter`, not the 30+ call sites

`TicketSyncService` has **zero** direct `AppController` knowledge — it talks exclusively to
`ITicketSyncDeps&` (`TicketSyncService.h:33`, `ITicketSyncDeps.h:33`). The interface already
carries every per-context channel Slice 1 needs:

- `Backend()` / `SetBackend()` / `BackendConnectivity()` — `ITicketSyncDeps.h:51–:57`
- `ActiveTicketsMutex()` / `ActiveTickets()` / `SetActiveTicketsPublished()` /
  `BumpActiveTicketsRevision()` — `:72–:78`
- `SyncWithBackend(const TrackerConfig*, const ViewsStore*)` already accepts an arbitrary
  `(config, views)` pair — `TicketSyncService.h:53`. **Plumbing feasibility: verified — no
  signature widening needed anywhere in the sync path.**

So the real change-site is: split `AppControllerDepsAdapter` into
`GridContextDepsAdapter(AppController& app, GridLiveContext& ctx)` — per-context methods
(`Backend`, `SetBackend`, `ActiveTickets*`) route to `ctx`; global methods (`Cache`,
connectivity banner, `PushOfflineReplayTimersDuringTransportOutage`, Lua notify) forward to
`app`. **`ITicketSyncDeps` itself is unchanged in Slice 1** → `tests/support/FakeTicketSyncDeps.h`
untouched → the Slice-0 regression net runs unmodified against both before/after states.

**Sync-completion routing needs NO paneId tagging at the deps layer**: each context owns its own
service + adapter, so a completion structurally lands in its own context. The plan-item-9
wording ("sync completion tagged with paneId, routed via MainThreadDispatcher") over-specifies —
what actually needs pane attribution is UI-facing cues/toasts (Slice 3), and the per-frame
`TickStreamingApply` driver (today called from the `SmatchetUI.cpp` frame path) becomes
`AppController::TickAllContexts()` iterating the map under one shared budget (Slice 3; trivial
loop in Slice 1 with one entry).

### 3.3 Call-site inventory (singleton assumptions) — and why Slice 1 touches none of them

Grep evidence (exhaustive, run in this pass):

- `GetActiveTicketsSnapshot()` — 32 call lines: 22 external (McpPlugin ×4, LuaConsolePlugin ×1,
  `BuiltinCommands_{Perf×3,Tickets×4,TicketMutations×1}`, UI ×8 across 6 files,
  `TrackerLabelsEditor` ×1) + 10 AppController-internal.
- `SyncWithBackend` / `IsStreamingSyncActive` / `TickStreamingApply` / `RefreshLocalData` — 72
  occurrences across 21 files; non-AppController callers concentrate in `ViewCommands.cpp` (4),
  `BuiltinCommands_Sync.cpp` (3), and 7 UI files.

**Strategy**: AppController keeps all these public methods as thin delegators into
`focusedContext()` (Slice 1: the single map entry). Zero external churn → behaviour-identical →
bisectable. They migrate per consumer in later slices: grid render takes `GridPane&` (S2);
commands/MCP/Lua grow an optional pane argument defaulting to focused (S4). The delegators are
the permanent "focused pane" semantics for global actions, not scaffolding to delete.

**Slice-1 commit plan (bisectability)**: (1) `GridLiveContext` extraction + delegators, no
behaviour change, Slice-0 net green; (2) `tickets_v2` migration; (3) pending-queue `BackendKey`.
Each independently green.

### 3.4 Shared-singleton thread-safety audit (N concurrent sync workers)

| Singleton | Evidence | Verdict under N workers | Cheapest sound fix |
|---|---|---|---|
| `LocalCacheManager` | `OPEN_FULLMUTEX` (`LocalCacheManager.cpp:93`); every cached-statement method serialised by `stmtMutex_` incl. the whole `SaveTickets` txn (`LocalCacheManager.h:130`, `.cpp:171`) | **already-safe** (serialised; contended but writes are batched + off-UI) | none for correctness. **Correctness hazard is semantic**: stale-deletion `DeleteTicket(id)` from pane A can delete pane B's row on numeric-id collision (GitHub `#123` vs Plane) — fixed by the `backend_key` scoping below, which is exactly why item 11 is front-loaded |
| `FieldCatalogCache` | process-wide file mutex around all 4 IO entry points (`FieldCatalogCache.cpp:18,446,496,546,574`); keys already `backend|endpoint|project` (`FieldCatalogCache.h:14`) | **already-safe** | none |
| `BackendAuditTrail` | single writer thread + mutex-guarded bounded queue, drop-oldest (`BackendAuditTrail.cpp:174–:287`) | **already-safe** for N producers | none; optionally stamp `backendKey` into event `Data` for attribution (S4 nice-to-have) |
| `ConfigManager` | cache mutex + RMW mutex + IO mutex + `ScopedFileLock` (`ConfigManager.cpp:505–:511,:1155`; `ConfigManager_Views.cpp:242–:243`); workers receive `TrackerConfig` **copies** (`TicketSyncService.h:85 cfgCopy`) | **already-safe** | none. New work is functional, not safety: per-pane `TrackerConfig` resolution from `backendKey` (item 13's `ConfigManager_Panes`) |
| `MainThreadDispatcher` | mutex-guarded, bounded 4096, drop-oldest, 4 ms drain budget, deferred tail requeues at front (`MainThreadDispatcher.h:42–:106`) | **already-safe** for N producers | none for safety. Fairness (one pane's burst starving others) is bounded by front-requeue FIFO; S3 adds the per-pane round-robin only if the `concurrent-sync` perf scenario shows starvation — don't pre-build it |

**Net audit result**: every shared singleton is already mutex-sound; the genuine Pillar-3 risks
are (a) cross-backend ticket-id collision in the flat cache (fixed by 3.5) and (b) the single
in-memory field catalog (deferred to S3, flagged in 3.1). TSan at S5 stays mandatory as the
proof, not the fix.

### 3.5 Cache namespacing + offline-queue BackendKey schema

**`backendKey` canonical form**: the output of
`ConfigManager::NormalizeViewsBackendKey(cfg.TrackerType)` (`Config/ConfigManager.h:640`) —
consistent with the views-v2 buckets panes already reference; forward-compat to
`<type>:<profileId>` documented in the ADR (string column, no schema change needed later).

**Tickets — new-table migration (NOT ADD COLUMN)**: the namespacing changes the PK (`id` →
`(backend_key, id)`), and SQLite cannot alter a PK in place; the Persistence invariant
(additive-only, no DROP/RENAME — `Source/Core/src/Persistence/AGENTS.md`) forces a versioned
step:

```sql
CREATE TABLE IF NOT EXISTS tickets_v2 (backend_key TEXT NOT NULL, id TEXT NOT NULL,
  PRIMARY KEY(backend_key, id));
CREATE TABLE IF NOT EXISTS ticket_field_values_v2 (backend_key TEXT NOT NULL,
  ticket_id TEXT NOT NULL, field_key TEXT NOT NULL, field_value TEXT,
  PRIMARY KEY(backend_key, ticket_id, field_key));
-- ticket_field_rich_values_v2: identical shape
```

One-time copy migration gated on the existing `cache_meta` flag helpers
(`LocalCacheManager.h:59–:60`): copy legacy rows stamping `backend_key =` the
currently-configured backend's key; legacy v1 tables retained on disk, unused
(additive-compliant; revisit-drop is a future versioned step). All `LocalCacheManager` ticket
methods gain a `backendKey` first parameter (`SaveTickets`, `TryGetTicket`, `DeleteTicket`,
`ForEachTicket`, `GetAllTicketIds` — `LocalCacheManager.h:35–:47`); Slice 1 callers pass the
single context's key. PK covers the per-backend lookups — **no additional indices needed**.

**Pending queues — ADD COLUMN (PK unchanged, autoincrement id)**: guarded
`ALTER TABLE ... ADD COLUMN backend_key TEXT NOT NULL DEFAULT ''` on `pending_creates`,
`pending_field_edits` + both dead twins, following the existing `SqliteTableHasColumn` pattern
(`LocalCacheManager.cpp:12,:39–:56,:135`). One-time stamp migration:
`UPDATE ... SET backend_key = <current key> WHERE backend_key = ''` under a `cache_meta` flag
(legacy rows were necessarily queued against the then-only backend). `PendingCreate` /
`PendingFieldEditRecord` (+ `DeadPending*`) gain `std::string BackendKey;`
(`CachedTicketTypes.h:51,:63`).

**Replay-matching rule**: a context's replay tick processes only rows
`WHERE backend_key = ctx.backendKey`. Rows whose backend has no live context **stay queued**
(never replayed against a wrong backend, never dropped), surfaced in the offline-queue UI with
their backend tag. Post-stamp, `backend_key = ''` cannot occur; if encountered
(corrupt/hand-edited DB) treat as no-match + WARN.

## 4. Risks

- **ABI/save-format**: the ticket-cache v2 migration touches every user's SQLite file — ASan run
  after S1 is already planned; add a Bucket-A round-trip test on a pre-migration fixture DB
  (legacy → v2 copy → read-back equality).
- **Backend leakage into core**: none introduced — `backendKey` is an opaque string in
  `Source/Core`; no backend-specific code crosses `ITrackerBackend`.
- **Dual-target**: `GridLiveContext.h` includes only existing Core headers (no GLFW/GL) — DX12
  path unaffected; dual-target build gate already in the plan.
- **Deferred catalog singleton** (3.1) — the one item where "Slice 1 done" ≠ "two live backends
  safe". Named row added to plan item 17 in this PR, else S3 ships a semantic catalog race.
- **Plan item 9 over-specification** — paneId-tagged dispatcher routing for sync applies is
  unnecessary at the deps layer (3.2); implementing it as written would add a channel that
  structurally already exists. Plan revised in this PR to "per-context adapter routes
  structurally; paneId attribution only for UI cues + shared tick budget".
- **MCP/Lua wire format**: unchanged in Slice 1 (delegators); S4 adds optional pane params —
  additive.

## 5. Implementation handoff

| Slice-1 sub-slice | Agent | Write scope | Pre-resolved decisions |
|---|---|---|---|
| 1a — `GridLiveContext` extraction + per-context adapter + delegators | `grid-engine` (or `mechanic` for the mechanical member moves) | `Source/Core/include/GridLiveContext.h`, `AppController.{h,cpp}`, `AppControllerDepsAdapter.{h,cpp}` | `map<int, unique_ptr<GridLiveContext>>`; `ITicketSyncDeps` unchanged; graveyard stays in AppController; catalog block does NOT move |
| 1b — tickets_v2 migration | Persistence owner | `LocalCacheManager.{h,cpp}` — tickets only | new tables + cache_meta-gated copy; legacy tables retained; method signatures gain `backendKey` |
| 1c — pending-queue BackendKey + replay rule | Persistence + Sync owners (two subsystems → two PR-able commits: columns/POD vs replay matcher) | `LocalCacheManager.cpp`, `CachedTicketTypes.h`, `OfflineQueueService.cpp` | ADD COLUMN + stamp migration; strict replay equality; unmatched rows stay queued |

Verification buckets (per AGENTS.md § Verification automation): all Slice-1 additions are
**bucket A** (migration round-trip on fixture DB; namespaced-cache disjointness; replay
BackendKey match; delegator behaviour-identity via the Slice-0 net) + **bucket D** (ASan run
post-1b). No bucket-E need in Slice 1 (no UI change).

Implementer contract: per AGENTS.md § Plan revision after implementation, the shipping agent
appends § Implementation log / Deviations / Verification to the plan-doc; the architect does
not.

## 6. Open questions

1. **Legacy v1 ticket tables** — retain indefinitely or schedule a versioned drop after one
   release cycle? (Persistence invariant says no DROP; a `cache_meta`-versioned drop step would
   need an explicit invariant amendment — propose deciding in the S5 PR.)
2. **`focusedContext()` when the focused pane's context is torn down mid-frame** (S3 concern,
   but the delegator contract should state it now): return the lone context in Slice 1; S3 must
   define fallback (first visible pane) before delegators can dangle.
3. **Should `GetAllTickets()`/`ForEachTicket` ever serve cross-backend (un-scoped) reads** for
   tooling/MCP, or is every read backend-scoped post-migration? Recommend: scoped-only, with an
   explicit `ForEachTicketAllBackends` if a real consumer appears.

## Implementation log

- `af465eb8` · #940 — Slice-1 design addendum + ADR-0018 (architect pass; this doc landed).
- #945 · 1a — `GridLiveContext` extraction + per-context adapter + delegators; `ITicketSyncDeps` unchanged, graveyard stays in AppController, catalog block not moved.
- #948 · 1b — `tickets_v2` family migration in `LocalCacheManager` (cache_meta-gated copy; legacy v1 tables retained).
- #951 · 1c — pending-queue `BackendKey` (ADD COLUMN + stamp migration) + scoped replay matching (`OfflineQueueService`).
- Later slices merged on the same line: #962, #975, #1010, #1013, #1156.

## Deviations from plan

- Field-catalog block (3.1 — `TrackerFieldCatalogRevision` / `AvailableFields` / `AvailableComponents` / `AvailableIssueTypeMeta` / catalog keys) and paneId sync-completion attribution (3.2) were correctly punted to Slice 3, landing in #975 — matching the plan's flagged-deferral, not silent scope drop.

## Verification (actual)

Archival audit 2026-06-16 confirmed the Slice-1 deliverables present in the tree:
`GridLiveContext.{h,cpp}`, `ConfigManager_Panes.cpp`, `BackendKey` in `CachedTicketTypes.h`, the `tickets_v2` family in `LocalCacheManager`, and `ADR-0018`. All verified present in tree (archival audit 2026-06-16), not re-run (no build/test suite re-executed for this archival).
