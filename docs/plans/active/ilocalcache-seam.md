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

Introduce a narrow `ILocalCache` pure-virtual interface in `Source/Core/include/Persistence/` carrying **only the ~30 methods the two services actually call** (not the full `LocalCacheManager` API — chat, migration, schema, and AppController-direct ticket ops stay off the interface and keep calling the concrete class). `LocalCacheManager` gains `: public ILocalCache` and marks those methods `override`; its constructor and SQLite-specific surface are unchanged, so every existing concrete caller (AppController, the impl tests) is unaffected. The two service-deps seams (`IOfflineQueueDeps::Cache()`, `ITicketSyncDeps::Cache()`) and the production `GridContextDepsAdapter::Cache()` change their return type from `LocalCacheManager*` to `ILocalCache*` — a mechanical, behavior-neutral retype since `LocalCacheManager` now IS-A `ILocalCache`.

A header-only `tests/support/FakeLocalCache.h` implements `ILocalCache` over `std::unordered_map` / `std::vector`, replacing `LocalCacheManager(":memory:")` inside `FakeOfflineQueueDeps` / `FakeTicketSyncDeps`. The **central risk** is fake-vs-real semantic drift (some methods — `ResolveFieldEditConflict`, `ArchivePendingCreate` dead-lettering, backend-key namespacing — carry non-trivial logic). The non-obvious trade-off that shaped the design: rather than trust a hand-written fake, a **shared contract-test suite** runs the same assertions against *both* `FakeLocalCache` and a real `:memory: LocalCacheManager`, so the fake can't silently diverge from the production contract.

## Files to modify

**New (interface + fake):**
1. `Source/Core/include/Persistence/ILocalCache.h` — NEW pure-virtual interface; ~30 service-consumed methods; includes only `CachedTicketTypes.h` + the pending-queue POD headers (no SQLiteCpp). Virtual dtor; `= 0` on every method.
2. `tests/support/FakeLocalCache.h` — NEW header-only in-memory `ILocalCache`; per-instance maps so each test starts empty; replicates the behavioral contract (dead-letter archive, conflict-resolution outcome, backend-key namespacing) the services depend on.
3. `tests/Core/LocalCacheContract.test.cpp` — NEW contract suite: a templated/parameterised body asserting the `ILocalCache` contract, instantiated against **both** `FakeLocalCache` and real `LocalCacheManager(":memory:")` (the latter stays in the SQLite-linked set). Guards fake fidelity.

**Production seam (strict-zone — `Source/Core/src/{Sync,Persistence}` + matching include):**
4. `Source/Core/include/Persistence/LocalCacheManager.h:31` — add `: public ILocalCache`; `override` the ~30 interface methods (signatures unchanged); include `ILocalCache.h`.
5. `Source/Core/include/IOfflineQueueDeps.h:27,37` — fwd-decl `class ILocalCache;` (was `LocalCacheManager`); `Cache()` returns `ILocalCache*`.
6. `Source/Core/include/ITicketSyncDeps.h:25,50` — same retype; keep `CachedTicketTypes.h` include for the POD return types, drop the `LocalCacheManager.h` include if no longer needed.
7. `Source/Core/include/GridContextDepsAdapter.h` + `Source/Core/src/GridContextDepsAdapter.cpp` — production `Cache()` returns `ILocalCache*` (returns the app-owned concrete LCM, which IS-A ILocalCache).
8. `Source/Core/src/Sync/OfflineQueueService.cpp` — no logic change; verify includes (prefer `ILocalCache.h` / `CachedTicketTypes.h` over `LocalCacheManager.h`).
9. `Source/Core/src/Sync/TicketSyncService.cpp` — same as #8.

**Tests repointed to the fake (service tests → pure-logic):**
10. `tests/support/FakeOfflineQueueDeps.h` — `CacheImpl` becomes `FakeLocalCache` (was `LocalCacheManager(":memory:")`); `Cache()` returns `ILocalCache*`.
11. `tests/support/FakeTicketSyncDeps.h` — same.
12. `tests/CMakeLists.txt` — move the **service** TUs (OfflineQueue*, `TicketSyncService.test.cpp`, `IssueCreatePipelineIntegration.test.cpp`, `BackendSwitchRace1081.test.cpp`, `OfflineFieldEditMerge.test.cpp`) out of the SQLiteCpp-linked group into the pure-logic group; **keep** `LocalCacheManager*.test.cpp`, `LocalCacheTicketsV2Migration.test.cpp`, and the contract suite's real-LCM instantiation in the SQLite-linked group. Update the hand-maintained source list + the "add each new test" guard accordingly.

**Docs:**
13. `.coderabbit.yaml` — tighten the `tests/**` carve-out: now only the *impl* TUs legitimately use SQLite; note the service tests are pure.
14. `docs/self-improvement/categories/tooling.md` — mark the 2026-06-09 entry resolved (link this plan).

## Existing utilities reused

- `CachedTicket` / `PendingCreate` / `PendingFieldEdit` PODs — `Source/Core/include/CachedTicketTypes.h` (already SQLite-free; the precedent for this exact decoupling). The interface signatures reuse these verbatim.
- `GridContextDepsAdapter::Cache()` — `Source/Core/src/GridContextDepsAdapter.cpp:~70` — the single production seam point; only its return type changes.
- `OfflineQueueTestEnv.h` `TestEnvGuard` — `tests/support/OfflineQueueTestEnv.h` — retained only for the real-LCM contract instantiation + impl tests; service tests no longer need its temp-dir/config setup once on the fake.
- doctest `TEST_CASE_TEMPLATE` — for the dual-impl contract suite (#3).

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
- **RISK — interface surface creep:** if the ~30-method surface is mis-scoped, the build breaks (missing `override`) or the interface bloats. **Mitigation:** surface is grep-derived from actual `Cache()->` call sites (locked at plan time); compiler enforces completeness.
- **RISK — strict-zone churn (`Sync`/`Persistence`):** any violation fails CI. **Mitigation:** changes are mechanical retypes + `override` tags; no logic edits; dual-target build + full lint gate before push.
- **RISK — `CachedTicket` include graph:** `ITicketSyncDeps.h` pulls `LocalCacheManager.h` for `CachedTicket`. **Mitigation:** repoint to `CachedTicketTypes.h` (already exists for exactly this).
- **Non-goal — integration test lane:** the rejected alternative (new `SmatchetIntegrationTests` target). Not pursued; the seam makes a separate lane unnecessary for the service tests.
- **Non-goal — full LCM API on the interface:** only service-consumed methods; AppController/chat/migration keep the concrete class.
- **Non-goal — making `LocalCacheManager*.test.cpp` pure:** those test SQLite itself and stay SQLite-backed by design.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: the repointed service TUs (OfflineQueue*, TicketSyncService, BackendSwitchRace1081, …) build + pass with **no SQLiteCpp link** (prove via the CMake group move + a clean configure). New `LocalCacheContract.test.cpp` passes against **both** `FakeLocalCache` and real `:memory:` LCM (fidelity gate). Full rig green (the #1104 baseline was 1524/1524).
- **Bucket E (ImGui Test Engine)**: N/A — no UI change.
- **Bash-driver scenario / screenshot / sanitizer**: run the offline-replay + backend-swap scenarios under the ASan/UBSan presets to confirm the retype introduced no lifetime regression (the #1081 race path runs through this seam).
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
