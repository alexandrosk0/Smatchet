# Shared ownership of the active tracker backend

# Status

**Accepted (2026-05-31)** — drives slice S1 of [`memory-budget-and-lifetime-hardening`](../plans/active/memory-budget-and-lifetime-hardening.md). Surfaced by a `grill-with-docs` session that overturned the plan's original "site #6 is a shutdown-only hazard" framing.

# Context

`AppController::Backend` is a `std::unique_ptr<ITrackerBackend>`. It is created once in `AppController::Initialize` (`Source/Core/src/AppController.cpp:1258`, `Backend = backendFactory_->Create(activeTracker)`) **and reassigned live on a tracker switch**: Preferences → Save & Sync → `AppController::SyncWithBackend` → `TicketSyncService::SyncWithBackend`, which on a tracker-kind change calls `deps_.SetBackend(BackendFactory()->Create(<type>))` (`Source/Core/src/Sync/TicketSyncService.cpp:468/472/476`) → `AppControllerDepsAdapter::SetBackend` → `app_.Backend = std::move(backend)` (`Source/Core/src/AppControllerDepsAdapter.cpp:58`). The `unique_ptr` move-assign **frees the old backend object**. This runs synchronously on the UI thread.

Several fire-and-forget UI workers capture a raw pointer into the live backend and dereference it off-thread. The clearest is `SmatchetProjectPicker` (`Source/Core/src/Ui/SmatchetProjectPicker.cpp:101`), which captures `ITrackerConnectivity* clientPtr = client;` (where `client == &Backend->Connectivity()`) and calls `clientPtr->ListProjects()` (a network call, seconds long) on a detached thread. If the user switches Jira → Plane while that fetch is in flight, the old backend is freed and `clientPtr` dangles — a use-after-free.

The memory-hardening plan's S1 migrates these detached threads onto the joined `LaunchBackgroundTask` pool. That pool is joined only at shutdown (`~AppController` / `JoinBackgroundTasks`), so it closes the **shutdown** UAF window but **not** the **live-swap** window — `SetBackend` does not join in-flight workers. Live tracker switching is a shipped feature that must keep working, so blocking or restart-gating the swap is not acceptable.

# Decision

Make `AppController::Backend` a `std::shared_ptr<ITrackerBackend>`. Background workers that call into the backend capture a `shared_ptr<ITrackerBackend>` **copy**; a live `SetBackend` swap then drops only the controller's reference, and the old backend stays alive until the last in-flight worker releases its copy — the standard ownership-extends-lifetime guarantee, no extra coordination code.

- **All `Backend` reads go through `std::atomic_load` and all writes through `std::atomic_store`** (`BackendShared()`, `GetTrackerBackend*()`, the ~20 per-method latches in `AppController_CatalogAndFieldEdit.cpp`, and the two writes in `Initialize` + `AppControllerDepsAdapter::SetBackend`). A `shared_ptr` *instance* is not itself safe to copy/assign concurrently in C++14, so the latch read must be synchronized against the live-swap write — otherwise the latch operation itself is a data race (CodeRabbit #657). C++14 free functions; the project is `CMAKE_CXX_STANDARD 14`.
- `SetBackend` keeps its `std::unique_ptr<ITrackerBackend>` parameter (wrapped into a `shared_ptr` for `atomic_store`), so the call site is otherwise unchanged.
- `ITrackerBackendFactory::Create` is unchanged (still returns `unique_ptr`).
- The ~22 `Backend->…` call sites are unchanged (`operator->` is identical for `shared_ptr`).
- A new `std::shared_ptr<ITrackerBackend> AppController::BackendShared() const` accessor hands the strong reference to worker-spawning UI. `SmatchetProjectPicker::Draw` takes `AppController& app` (its 2 callers pass `app`, not a `shared_ptr` — keeping the function arity at 6 preserves its grandfathered function-size key) and derives `auto backend = app.BackendShared();` internally; the picker's pooled fetch captures that `shared_ptr` and calls `backend->Connectivity().ListProjects()`.

# Considered options

- **(a) Shared ownership** — chosen for the parts where consumers can cheaply hold a strong handle (the picker, the catalog/field-edit methods). Strong `shared_ptr` latches give those an automatic lifetime guarantee.
- **(b) Join in-flight workers inside `SetBackend` before freeing.** Rejected: `SetBackend` runs on the UI thread; joining a network fetch there is a Pillar-2 (>100 ms UI-thread block) violation, and could hang the swap for the full HTTP timeout.
- **(c) Deferred-free "graveyard"** — retire old backends into a holding list, freed at shutdown. **Initially rejected, then ADOPTED (CodeRabbit #657).** The exposure proved systemic: `AppControllerDepsAdapter` hands out *raw* subobject pointers (`Reader()`/`Mutations()`/`Connectivity()`), and `OfflineQueueService` captures them into a worker that outlives a live swap — consumer-latching can't cheaply cover every such raw-pointer holder. Retiring the old backend (kept alive until `~AppController`, after `JoinBackgroundTasks`) makes *all* those raw pointers safe in one place. Tracker switches are rare user actions, so the graveyard holds a handful of small objects per session — a deliberate, bounded leak, not unbounded growth. **(a) + (c) together** are the shipped design: atomic access closes the `shared_ptr`-instance data race; the graveyard closes the raw-subobject-pointer dangle.

# Consequences

- **Pillar 3 (never crash):** the live-swap UAF on the project-picker fetch (and the same class for any worker that captures a backend handle) is closed without serializing or blocking the swap.
- **Ownership clarity:** `Backend` now expresses "shared while in-flight work may reference it," not "sole owner." A future reader sees `shared_ptr` and the ADR explains why it is not `unique_ptr`.
- **No interface churn:** `SetBackend(unique_ptr)`, the factory, and every `Backend->…` access compile unchanged; the only new surface is `BackendShared()` + the picker `Draw` signature.
- **Verification shift:** site #6's repro is **not** "shut down mid-fetch" — it is "start a project-picker fetch, then switch tracker live." The S1 sanitizer (bucket-D) repro is updated accordingly (TSan/ASan: kick the picker fetch, fire a live `SetBackend`, assert no UAF/data-race), in addition to the shutdown-mid-flight repro for the other five sites.
- **Scope note:** a worker that dereferences `app.Backend` across a blocking call must latch a **local `shared_ptr` copy** for the call's duration — reading the member fresh is not enough, because the swap can free the object mid-call. **Every method in `AppController_CatalogAndFieldEdit.cpp` that derefs `Backend` now latches a local copy at entry** — the `FetchFieldCatalog` overloads, `RefreshFieldCatalog`, `EnsureIssueEditMetaLoaded`, the `SubmitFieldEdit*` mutation paths, and the watcher/vote/comment/worklog/user-search Collaboration calls (~20 methods). `AppController.cpp`'s own worker-reachable readers — `PrefetchIssueTicketsForKeys` and `FetchIssuesForActiveView` (callable off-thread via MCP/Lua command dispatch) — latch via `std::atomic_load` too (CodeRabbit #657 rd3). Any future worker-reachable `Backend` read should do the same.
