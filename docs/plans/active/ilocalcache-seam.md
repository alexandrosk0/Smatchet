# Plan — ILocalCache seam for pure-logic service tests

> **Slug**: `ilocalcache-seam` (matches this file's basename without `.md`).
>
> **Status**: `active`
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules.

## Context

The `tests/Core` doctest rig is documented as **pure-logic only** (`.coderabbit.yaml` `tests/**` instruction; `test-rig` agent), but ~8 TUs drive real SQLite because `OfflineQueueService` and `TicketSyncService` take a **concrete `LocalCacheManager*`** — there is no cache interface to inject a fake, so the only way to exercise the services is with a real (`:memory:`) cache. This drift surfaced on PR #1104 as a CodeRabbit "compliance break" finding; the immediate fix (PR #1107 / commit `7041d1bd`) was to carve `:memory:` SQLite into the `.coderabbit.yaml` rule and file this follow-up (`docs/self-improvement/categories/tooling.md`, 2026-06-09, P2).

This plan extracts an `ILocalCache` interface so the **service** tests become genuinely pure-logic (inject an in-memory fake, no SQLiteCpp link), while the cache *implementation* tests deliberately stay SQLite-backed (they validate the real SQLite class). **After this lands**: the offline-queue / ticket-sync service test surface is SQLite-free and the doctest rig's "no SQLite" rule is true for everything except the handful of TUs that exist to test SQLite itself.

Originating: PR #1104 / #1107 closeout; backlog `tooling.md` 2026-06-09 (P2).

## Approach

Introduce a narrow `ILocalCache` pure-virtual interface in `Source/Core/include/Persistence/` carrying **exactly the 28 methods the seam consumers call** (pinned in § Interface method inventory below — 27 from the two services + `TryGetTicket` from `IssueCreatePipeline`, which receives the seam cache via `OfflineQueueService.cpp:1270`; NOT the full `LocalCacheManager` API — chat, migration, schema, and AppController-direct ticket ops stay off the interface and keep calling the concrete class). `LocalCacheManager` gains `: public ILocalCache` and marks those methods `override`; its constructor and SQLite-specific surface are unchanged, so every existing concrete caller (AppController, the impl tests) is unaffected. The retype propagates along the call chain: the two service-deps seams (`IOfflineQueueDeps::Cache()`, `ITicketSyncDeps::Cache()`), the production `GridContextDepsAdapter::Cache()`, **`IssueCreatePipeline::Run`'s `cache` param**, and `OfflineQueueService`'s 8 private helper params + 2 locals — all `LocalCacheManager*` → `ILocalCache*`. Behavior-neutral since `LocalCacheManager` IS-A `ILocalCache` (concrete callers like `AppController_IssueCreateOffline.cpp` upcast implicitly).

A header-only `tests/support/FakeLocalCache.h` implements `ILocalCache` over `std::unordered_map` / `std::vector`, replacing `LocalCacheManager(":memory:")` inside `FakeOfflineQueueDeps` / `FakeTicketSyncDeps` (+ `SqliteMemFixture` usage in `IssueCreatePipelineIntegration.test.cpp`). The **central risk** is fake-vs-real semantic drift (some methods — `ResolveFieldEditConflict`, `ArchivePendingCreate` dead-lettering, backend-key namespacing — carry non-trivial logic). The non-obvious trade-off that shaped the design: rather than trust a hand-written fake, a **shared contract-test suite** runs the same assertions against *both* `FakeLocalCache` and a real `:memory: LocalCacheManager`, so the fake can't silently diverge from the production contract.

**What "pure" means here (deliverable, precisely):** `SmatchetTests` is ONE executable with target-wide linkage (`tests/CMakeLists.txt:683-695`) — SQLiteCpp cannot leave the link line while the impl tests + the contract suite's real half exist, and `TicketSyncService.cpp` is ImGui-coupled (Toast). So the enforceable claim is **include/construction purity, not link purity**: after this plan, no service-test TU or shared fake *constructs* `LocalCacheManager` or *transitively includes* `<SQLiteCpp/...>`. That is gated by a grep check (§ Verification), not by a second CTest target (rejected non-goal, reaffirmed after review finding C1).

## Interface method inventory (locked — the complete `ILocalCache` surface, 28)

All non-const, signatures per `Source/Core/include/Persistence/LocalCacheManager.h`:

- *Tickets (TicketSyncService, 5)*: `SaveTicket(const std::string&, const CachedTicket&)` · `SaveTickets(const std::string&, const std::vector<CachedTicket>&)` · `DeleteTicket(const std::string&, const std::string&)` · `GetAllTickets(const std::string&) → std::vector<CachedTicket>` · `GetAllTicketIds(const std::string&) → std::vector<std::string>`
- *Pending creates (OfflineQueueService, 10)*: `EnqueuePendingCreate(const std::string&, const std::string&) → std::int64_t` · `LoadPendingCreates() → std::vector<PendingCreate>` · `UpdatePendingCreate(std::int64_t, int, const std::string&)` · `DeletePendingCreate(std::int64_t)` · `ArchivePendingCreate(std::int64_t, const std::string&, const std::string&)` · `UpdatePendingCreatePayload(std::int64_t, const std::string&)` · `LoadDeadPendingCreates() → std::vector<DeadPendingCreate>` · `GetDeadPendingCreateCount() → size_t` · `RestoreDeadPendingCreate(std::int64_t) → bool` · `DeleteDeadPendingCreate(std::int64_t)`
- *Pending field-edits (OfflineQueueService, 10)*: `EnqueuePendingFieldEdit(const std::string&, const std::string&, const std::string&, const std::string&, const std::string& = "", const std::string& = "", bool = false) → std::int64_t` · `LoadPendingFieldEdits() → std::vector<PendingFieldEditRecord>` · `UpdatePendingFieldEdit(std::int64_t, int, const std::string&)` · `DeletePendingFieldEdit(std::int64_t)` · `MarkFieldEditConflict(std::int64_t, const std::string&)` · `ResolveFieldEditConflict(std::int64_t, const std::string&)` · `ArchivePendingFieldEdit(std::int64_t, const std::string&, const std::string&)` · `LoadDeadPendingFieldEdits() → std::vector<DeadPendingFieldEdit>` · `RestoreDeadPendingFieldEdit(std::int64_t) → bool` · `DeleteDeadPendingFieldEdit(std::int64_t)`
- *Meta flags (OfflineQueueService, 2)*: `HasCacheMetaFlag(const std::string&) → bool` · `SetCacheMetaFlag(const std::string&)`
- *Via IssueCreatePipeline (1)*: `TryGetTicket(const std::string&, const std::string&, CachedTicket&) → bool` (also called 16× by `TicketSyncService.test.cpp` through `CacheImpl`).

**C++14 hazard (record now, enforce at review):** `EnqueuePendingFieldEdit` carries default arguments — defaults on virtuals bind statically, so DIVERGENT defaults silently fork the API. Resolution: keep **identical** defaults on all three declarations (`ILocalCache`, `LocalCacheManager` override, `FakeLocalCache` override) with a review pin that they must match. (Default-free overrides were considered and rejected: the short-arity callers are *concrete-typed* — `LocalCacheManager.test.cpp` ~7 five-arg sites, `OfflineQueueBackendKey.test.cpp:239` four-arg via `CacheImpl` — so stripping defaults breaks them; the sole seam-side production call, `OfflineQueueService.cpp:527`, passes all 7 args explicitly and is indifferent.) All POD return/param types live in the SQLite-free `CachedTicketTypes.h` (all five: `CachedTicket`, `PendingCreate`, `PendingFieldEditRecord`, `DeadPendingCreate`, `DeadPendingFieldEdit`) — `ILocalCache.h` includes exactly that one header.

## Files to modify

**New (interface + fake):**
1. `Source/Core/include/Persistence/ILocalCache.h` — NEW pure-virtual interface; the 28 inventory methods; includes **only** `CachedTicketTypes.h` (verified sufficient — all five PODs live there, no SQLiteCpp). Virtual dtor; `= 0` on every method; `EnqueuePendingFieldEdit` defaults identical to the concrete impls (see hazard above).
2. `tests/support/FakeLocalCache.h` — NEW header-only in-memory `ILocalCache`; per-instance maps so each test starts empty; replicates the behavioral contract (dead-letter archive, conflict-resolution outcome, backend-key namespacing, `TryGetTicket`) the services + pipeline depend on.
3. `tests/Core/LocalCacheContract.test.cpp` — NEW contract suite: `TEST_CASE_TEMPLATE` (doctest v2.4.11, C++14-clean) over per-impl maker/fixture wrapper types (the two impls construct differently: `FakeLocalCache{}` vs `LocalCacheManager(":memory:")`), asserting identical contract behavior against **both**. Guards fake fidelity. This TU transitively includes SQLiteCpp by design (real half).

**Production seam (strict zones: `Sync`, `Persistence`, `Tracker` + matching includes — retype chokepoint is `OfflineQueueService.{h,cpp}` + `IssueCreatePipeline.{h,cpp}`, not just the seam getters):**
4. `Source/Core/include/Persistence/LocalCacheManager.h:31` — add `: public ILocalCache`; `override` the 28 inventory methods (signatures unchanged; `EnqueuePendingFieldEdit` keeps its defaults, identical to the interface's); include `ILocalCache.h`.
5. `Source/Core/include/IOfflineQueueDeps.h:27,37` — fwd-decl `class ILocalCache;` (was `LocalCacheManager`); `Cache()` returns `ILocalCache*`; reword the line-35 "Local SQLite cache" doc-comment backend-neutral.
6. `Source/Core/include/ITicketSyncDeps.h:25,50` — retype; **swap** the `LocalCacheManager.h` include (line 25, present today for `CachedTicket`) for `CachedTicketTypes.h` — this include swap is load-bearing for include-purity, not cosmetic.
7. `Source/Core/include/Sync/OfflineQueueService.h:49,203,215,222,230,238,242,247,275` — fwd-decl swap + retype the **8 private helper params** from `LocalCacheManager*` to `ILocalCache*`.
8. `Source/Core/src/Sync/OfflineQueueService.cpp:797,877,941,977,1021,1058,1086,1161,1217,1375` — matching helper-definition retypes + 2 concrete-typed locals; include swap to `ILocalCache.h`. (Line 1270 passes the seam cache into `IssueCreatePipeline::Run` — compiles only after #10.)
9. `Source/Core/include/Sync/TicketSyncService.h:21` — **swap** the `LocalCacheManager.h` include (today: "For CachedTicket inside StreamingSyncState") for `CachedTicketTypes.h`; without this, every service-test TU keeps transitively including SQLiteCpp and the purity gate fails.
10. `Source/Core/include/Tracker/IssueCreatePipeline.h:13,61` + `Source/Core/src/Tracker/IssueCreatePipeline.cpp:7,255,312` — fwd-decl/include swap + retype BOTH `cache` params: the `Run` definition (`.cpp:312`) AND the file-internal helper `RunUpdateExisting` (`.cpp:255`, called from `Run` at 326). The cache *calls* (`TryGetTicket` :297, `SaveTicket` :302,368) need no edit — both on the interface. Other `Run` callers stay source-compatible: `AppController_IssueCreateOffline.cpp:119,153` pass concrete LCM (implicit upcast), `BugReportService.cpp:543` (`Source/Core/src/Diagnostics/`) passes nullptr.
11. `Source/Core/include/GridContextDepsAdapter.h:49` + `Source/Core/src/GridContextDepsAdapter.cpp:25` — production `Cache()` returns `ILocalCache*` (returns `app_.Cache.get()`, the app-owned `std::unique_ptr<LocalCacheManager>` at `AppController.h:926` — upcast-clean).
12. `Source/Core/src/Sync/TicketSyncService.cpp` — include verification only (no signature changes; its 5 ticket calls + null-checks go through the deps seam).

**Tests repointed to the fake (the 7 `FakeOfflineQueueDeps`/`FakeTicketSyncDeps`-using TUs + the pipeline TU):**
13. `tests/support/FakeOfflineQueueDeps.h` — `CacheImpl` becomes `FakeLocalCache` (was `LocalCacheManager(":memory:")`); `Cache()` returns `ILocalCache*`.
14. `tests/support/FakeTicketSyncDeps.h` — same.
15. `tests/Core/IssueCreatePipelineIntegration.test.cpp` — rewrite off `SqliteMemFixture::Get()` (returns concrete LCM*) onto `FakeLocalCache` (~15 call sites; needs `TryGetTicket` on the fake).
16. `tests/CMakeLists.txt` — no link-group move exists (target-wide linkage); instead: register the new contract TU; ensure the repoint set — `BackendSwitchRace1081`, `OfflineQueueServiceRuntime`, `OfflineQueueTwoBackendReplay`, `OfflineQueueBackendSwap`, `OfflineQueueBackendKey`, `TicketSyncService`, **`TrackerBackendFactoryConfig`** (uses `FakeTicketSyncDeps` — easy to miss), `IssueCreatePipelineIntegration` — has no lingering `LocalCacheManager` construction; keep `LocalCacheManager*.test.cpp` / `LocalCacheTicketsV2Migration.test.cpp` / contract suite SQLite-backed. (`OfflineFieldEditMerge.test.cpp` is already pure — no action.) Also fix the pre-existing imprecise comment near `tests/CMakeLists.txt:680` claiming `IssueCreatePipeline.cpp` "pulls SQLite/Statement directly" — it's transitive via the LCM include, and goes away entirely after row #10's include swap.

**Docs:**
17. `.coderabbit.yaml:113-127` — the carve-out text asserts "no `ILocalCache` seam to fake", which this plan makes false — re-tighten: service tests are pure; `:memory:` SQLite remains legitimate ONLY for the LCM impl TUs + migration + the contract suite's real half.
18. `docs/self-improvement/categories/tooling.md:506` — mark the 2026-06-09 `.coderabbit.yaml`-drift entry resolved (link this plan).

## Existing utilities reused

- `CachedTicket` / `PendingCreate` / `PendingFieldEditRecord` / `DeadPendingCreate` / `DeadPendingFieldEdit` PODs — `Source/Core/include/CachedTicketTypes.h:13-136` (verified SQLite-free — includes only `<cstdint>/<string>/<unordered_map>`; the precedent for this exact decoupling). The interface signatures reuse these verbatim.
- `GridContextDepsAdapter::Cache()` — `Source/Core/src/GridContextDepsAdapter.cpp:25` (decl `GridContextDepsAdapter.h:49`) — the single production seam getter; only its return type changes.
- `OfflineQueueTestEnv.h` `TestEnvGuard` — `tests/support/OfflineQueueTestEnv.h` — retained only for the real-LCM contract instantiation + impl tests; service tests no longer need its temp-dir/config setup once on the fake.
- doctest `TEST_CASE_TEMPLATE` — pinned doctest v2.4.11 (`tests/CMakeLists.txt:8-11`), C++14-clean — for the dual-impl contract suite (#3).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — adds one virtual-call indirection on cache access; all cache calls are already off the UI thread (replay/sync workers). Negligible, not in any render path.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: no impact — no new sync I/O; the seam is a pure retype of existing off-thread calls.
- **Pillar 3 (never crash)**: virtual dtor on `ILocalCache` mandatory (interface deletion safety); fake uses RAII containers; net reduction in test reliance on real DB lifecycle.
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

Touches `Source/Core/` → gates apply, but the change is behavior-neutral (interface retype, no algorithm change):

1. **PR-fast CI** — scenario most exercising the path: the offline-replay / ticket-sync scenarios. No perf delta expected (one vtable indirection on already-off-thread calls). Run the named scenario per § Verification pre-push.
2. **Pillar 2 static scanner** — no new `ImGui::*`-reachable sync I/O. N/A.
3. **Dispatcher drain** — does not touch `MainThreadDispatcher::Drain()`. N/A.
4. **Visible-cue bucket-E harness** — no new >100 ms sync path. N/A.
5. **Marker inventory** — adds no `SMATCHET_UI_PERF_SCOPE` markers. N/A.

**Override**: not anticipated; if the vtable indirection shows a measurable regression, `perf-out-of-band` + baseline bump.

## Risks / non-goals

- **RISK — fake-vs-real semantic drift (central):** a hand-written `FakeLocalCache` could diverge from `LocalCacheManager`'s real semantics, so the "pure" tests would validate the fake, not production. **Mitigation:** the shared contract suite (#3) runs identical assertions against both impls; a service test only trusts the fake for behavior the contract suite pins.
- **RISK — interface surface creep / mis-scope:** mitigated — the surface is **pinned** in § Interface method inventory (verified call-site-by-call-site against `LocalCacheManager.h`, including the `IssueCreatePipeline` indirection a naive `Cache()->` grep misses); compiler enforces completeness via `override`.
- **RISK — default-args-on-virtual (C++14 static binding):** divergent defaults across `ILocalCache` / `LocalCacheManager` / `FakeLocalCache` would silently fork the API by call-site static type. **Mitigation:** identical defaults on all three declarations (concrete-typed test callers rely on them — see hazard note); review pin that they match; the contract suite exercises the defaulted arity through both impls.
- **RISK — strict-zone churn (now `Sync` + `Persistence` + `Tracker`):** any violation fails CI. **Mitigation:** changes are mechanical retypes + `override` tags; no logic edits; dual-target build + full lint gate before push.
- **RISK — `CachedTicket` include graph:** `ITicketSyncDeps.h:25` AND `TicketSyncService.h:21` pull `LocalCacheManager.h` for `CachedTicket` → transitively `<SQLiteCpp/...>` into every service-test TU. **Mitigation:** both swaps to `CachedTicketTypes.h` are explicit file rows (#6, #9); the purity gate (§ Verification) catches a missed one.
- **Non-goal — integration test lane / second CTest target:** the rejected alternative (new `SmatchetIntegrationTests` executable). Reaffirmed after review finding C1: `SmatchetTests` keeps target-wide SQLiteCpp linkage; this plan's claim is include/construction purity, not link purity.
- **Non-goal — link-level SQLite removal from `SmatchetTests`:** impossible while the impl tests + contract suite real-half exist in the single target; explicitly not claimed.
- **Non-goal — full LCM API on the interface:** only service-consumed methods; AppController/chat/migration keep the concrete class.
- **Non-goal — making `LocalCacheManager*.test.cpp` pure:** those test SQLite itself and stay SQLite-backed by design.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: the 8 repointed TUs (the 7 fake-users incl. `TrackerBackendFactoryConfig` + `IssueCreatePipelineIntegration`) build + pass on `FakeLocalCache`. New `LocalCacheContract.test.cpp` passes against **both** `FakeLocalCache` and real `:memory:` LCM (fidelity gate). Full rig green (the #1104 baseline was 1524/1524).
- **Include/construction purity gate (the deliverable's enforcement — replaces the unprovable "no SQLiteCpp link")**: a grep check asserting (a) no repointed TU or shared fake constructs `LocalCacheManager(` and (b) `gcc/clang -M`-style include trace (or a `grep -r 'SQLiteCpp' ` over the preprocessed includes of the repointed TUs) shows no `<SQLiteCpp/...>` reach. Wire it as a small bash check alongside the existing source-list guard in `tests/CMakeLists.txt` / `scripts/dev` so regression is CI-caught, not convention.
- **Bucket E (ImGui Test Engine)**: N/A — no UI change.
- **Bucket D (bash-driver scenario / sanitizer)**: run the offline-replay + backend-swap scenarios under the ASan/UBSan presets to confirm the retype introduced no lifetime regression (the #1081 race path runs through this seam).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target — `ILocalCache.h` must compile clean under DX12; no GLFW/GL leakage).
- **Doc validation (blocks plan-doc PRs)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs`**: stress-test this plan against the domain model (esp. the LCM contract surface + ADR-0012 backend-ownership) before finalising; record the outcome. **Not yet run — do before implementation.**
- **Manual residue**: none anticipated; if the contract suite can't cover a behavior the services rely on, file a `tooling.md` entry naming the gap.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray refs to the deferred "integration lane" / "ILocalCache" follow-up and revise/delete (the `tooling.md` 2026-06-09 entry is the known one).

- **`ITrackerBackend`-style shared ownership for the cache** — out of scope; the cache is single-owned by AppController. No-action.
- **Async/threaded cache interface** — the interface mirrors today's synchronous (off-thread-called) API; no async redesign. Follow-up only if a future need appears.
- **Migrating chat / schema-migration tests off SQLite** — they test SQLite behavior; intentionally unchanged.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*
