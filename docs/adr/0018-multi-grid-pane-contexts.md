# ADR 0018 — Per-pane GridLiveContext: de-singletoning the active tracker engine

- Status: proposed
- Date: 2026-06-06
- Extends: ADR-0012 (shared-ownership active tracker backend)
- Plan: docs/plans/multi-grid-tabs.md (+ [slice1-design addendum](../plans/active/multi-grid-tabs-slice1-design.md))

## Context

AppController owns exactly one backend / one TicketSyncService / one ActiveTickets
snapshot (AppController.h:970,:1000,:1011). Multi-grid panes require N concurrent
live (backend, sync, snapshot) bundles, with shared SQLite cache + offline queue.

## Decision

1. Extract the per-backend engine bundle into `GridLiveContext` (backend shared_ptr
   under ADR-0012 atomic discipline, TicketSyncService, ActiveTickets + published
   snapshot + revision, backendKey/catalogKey). AppController owns
   `map<paneId, unique_ptr<GridLiveContext>>`; public methods delegate to the
   focused context (permanent focused-pane semantics, not scaffolding).
2. The migration chokepoint is `AppControllerDepsAdapter` → per-context
   `GridContextDepsAdapter`; `ITicketSyncDeps` is unchanged, so sync, tests, and
   the ~30 external snapshot/sync call sites are untouched in the foundation slice.
3. One retired-backend graveyard stays in AppController, shared by all contexts
   (ADR-0012 semantics unchanged; RetireBackend already thread-safe).
4. Durable storage is namespaced by `backend_key` (= NormalizeViewsBackendKey
   output; forward-compat to `<type>:<profileId>`): tickets move to v2 tables with
   PK (backend_key, id) via a cache_meta-gated one-time copy (PK change cannot be
   done additively in place; legacy tables retained); pending queues gain an
   additive backend_key column + stamp migration; replay is strict
   backend_key-equality, unmatched rows stay queued.
5. Shared singletons (LocalCacheManager FULLMUTEX+stmtMutex, FieldCatalogCache
   file mutex, BackendAuditTrail writer thread, ConfigManager mutexes,
   MainThreadDispatcher) are audited mutex-sound for N producers — no per-context
   DB connections. The in-memory field-catalog block remains single-backend and
   MUST move per-context before two contexts go live (Slice 3).

## Consequences

- (+) Linear (or any new backend) slots in as "a backend a pane points at".
- (+) Foundation slice is behaviour-identical + bisectable; Slice-0 net unmodified.
- (−) N sync workers contend on one stmtMutex_ (accepted: batched, off-UI).
- (−) Legacy v1 ticket tables linger until a future versioned drop (open question
  in the design addendum).
- (−) Focused-pane delegators make "the" snapshot ambiguous for MCP/Lua until S4
  adds explicit pane addressing.
