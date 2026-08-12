# Plan — ITrackerClient Interface Split
<!-- plan-date: 2026-05-26 -->

> **Slug**: `tracker-interface-split`

## Context

`ITrackerClient` (`Source_Core/include/ITrackerClient.h:58`) is a 28-method god interface mixing issue read/sync, field catalog fetch, field payload building, issue create/update/attach, comments/watchers/votes/worklogs, user search, project discovery, and display-value formatting. Five backends implement it (JiraClient, PlaneClient, GitHubClient, plus two fixture backends and a test fake), each forced to stub methods they don't support via 16 default "unsupported" implementations.

The concrete harms: (1) call sites like `TicketSyncService` depend on the entire 27-method surface when they only call `FetchIssuesStreamed`; (2) fixture backends and `FakeTrackerClient` must override methods they never exercise; (3) adding a new backend (e.g. Linear) requires implementing or explicitly stubbing all 28 methods; (4) there is no compile-time signal that a call site uses only read methods or only mutation methods.

After this lands, call sites depend on the narrowest capability interface they need. Adding a backend that supports only read + catalog (no mutations, no collaboration) compiles without stubs. Test fakes implement only the interface under test.

## Approach

Split `ITrackerClient` into five capability interfaces by concern area, chosen to match the natural call-site clusters found in the codebase. Use multiple inheritance — each backend class inherits from the capability interfaces it supports. Call sites hold the narrow type. A top-level `ITrackerBackend` composes the capabilities with optional (nullable) accessors for features a backend may not implement.

The five interfaces and their method assignments are derived from the actual call-site map (23 distinct call sites across 8 files):

**`ITrackerIssueReader`** — read path + display:
- `GetTrackerType()`, `FetchIssues()`, `FetchIssuesStreamed()`, `FetchIssuesForKeys()`, `ResolveDisplayValue()`, `BuildBrowseUrl()`
- Consumers: `TicketSyncService` (1 call), `OfflineQueueService` (1 call), `AppController::FetchIssuesForActiveView` (1 call)

**`ITrackerConnectivity`** — health + project discovery:
- `ProbeReachability()`, `ExtractProjectFromQuery()`, `ListProjects()`
- Consumers: `AppController_Connectivity` (1 call), `ProjectResolver` (2 calls), `SmatchetBulkTicketsUi` (1 call), `SmatchetViewsDashboardUi_widgets` (1 call)

**`ITrackerFieldCatalog`** — schema fetch:
- `FetchFieldCatalog()`, `FetchIssueEditMeta()`
- Consumers: `AppController_CatalogAndFieldEdit::EnsureFieldCatalogLoaded` (3 calls)

**`ITrackerIssueMutations`** — write path:
- `UpdateIssueFields()`, `UpdateField()`, `BuildFieldPayload()`, `BuildCreatePayload()`, `BuildUpdatePayload()`, `CreateIssue()`, `AttachFilesToIssue()`, `AddIssueToSprint()`
- Consumers: `OfflineQueueService` (1 call), `IssueCreatePipeline` (7 calls), `AppController_CatalogAndFieldEdit::TryBuildFieldEditPayloadForNetwork` (1 call)

**`ITrackerCollaboration`** — social features:
- `AddIssueCommentPlain()`, `FetchIssueComments()`, `FetchIssueWatchers()`, `AddIssueWatcher()`, `FetchIssueVotes()`, `SearchUsersByQuery()`, `AddWorklog()`, `AddIssueCommentBlameContext()`, `FetchUserGroupNames()`
- Consumers: UI code in `SmatchetUI.cpp` draw paths (detail panel tabs), `BlameAnalysisUi` (blame-comment export)

The top-level `ITrackerBackend` returns capabilities by pointer (nullptr = unsupported):

```cpp
class ITrackerBackend {
public:
    virtual ~ITrackerBackend() = default;
    virtual ITrackerIssueReader& Reader() = 0;
    virtual ITrackerConnectivity& Connectivity() = 0;
    virtual ITrackerFieldCatalog* FieldCatalog() = 0;   // nullptr if unsupported
    virtual ITrackerIssueMutations* Mutations() = 0;     // nullptr if unsupported
    virtual ITrackerCollaboration* Collaboration() = 0;  // nullptr if unsupported
};
```

`Reader()` and `Connectivity()` return references (always available — every backend can read and probe). `FieldCatalog()`, `Mutations()`, and `Collaboration()` return pointers (nullptr when unsupported). This makes capability checks explicit at call sites (`if (auto* m = backend->Mutations()) { m->CreateIssue(...); }`).

Migrate incrementally with a compatibility facade:

1. **Slice 1 — Extract `ITrackerIssueReader` + `ITrackerConnectivity`**: highest-value split. `TicketSyncService` and `ProjectResolver` become narrow-typed. Old `ITrackerClient` inherits from both new interfaces as a facade.
2. **Slice 2 — Extract `ITrackerIssueMutations`**: `OfflineQueueService` and `IssueCreatePipeline` become narrow-typed. `AppController_CatalogAndFieldEdit::TryBuildFieldEditPayloadForNetwork` gets the narrow type.
3. **Slice 3 — Extract `ITrackerFieldCatalog` + `ITrackerCollaboration`**: remaining methods. `ITrackerClient` becomes an alias for `ITrackerBackend` or is deleted.
4. **Slice 4 — Delete the facade**: update `AppController::Backend` from `std::unique_ptr<ITrackerClient>` to `std::unique_ptr<ITrackerBackend>`. Update all remaining call sites that still use the wide type.

## Files to modify

**New headers (capability interfaces)**

1. `Source_Core/include/ITrackerIssueReader.h` — new file: `ITrackerIssueReader` pure virtual interface (6 methods).
2. `Source_Core/include/ITrackerConnectivity.h` — new file: `ITrackerConnectivity` pure virtual interface (3 methods).
3. `Source_Core/include/ITrackerFieldCatalog.h` — new file: `ITrackerFieldCatalog` pure virtual interface (2 methods).
4. `Source_Core/include/ITrackerIssueMutations.h` — new file: `ITrackerIssueMutations` pure virtual interface (8 methods).
5. `Source_Core/include/ITrackerCollaboration.h` — new file: `ITrackerCollaboration` pure virtual interface (9 methods).
6. `Source_Core/include/ITrackerBackend.h` — new file: `ITrackerBackend` composition interface with typed accessors.

**Modified headers (facade + backends)**

7. `Source_Core/include/ITrackerClient.h:58` — make `ITrackerClient` inherit from all five capability interfaces during transition; delete once migration completes.
8. `Source_Core/include/JiraClient.h:18` — inherit from `ITrackerBackend` (all five capabilities); implement accessor methods.
9. `Source_Core/include/PlaneClient.h:11` — same pattern as JiraClient.
10. `Source_Core/include/GitHubClient.h:18` — same; `Collaboration()` returns nullptr (GitHub backend doesn't implement watchers/votes/worklogs).
11. `Source_Core/include/GitHubFixtureBackend.h:32` — inherit from `ITrackerIssueReader` + `ITrackerIssueMutations` only.
12. `Source_Core/include/PlaneFixtureBackend.h:30` — same as GitHubFixtureBackend.
13. `tests/support/FakeTrackerClient.h` — narrow to `ITrackerIssueReader` + `ITrackerIssueMutations`.

**Modified call sites**

14. `Source_Core/src/TicketSyncService.cpp:533` — change `deps_.Backend()` return type from `ITrackerClient*` to `ITrackerIssueReader*`. Only calls `FetchIssuesStreamed`.
15. `Source_Core/include/TicketSyncService.h` — update `ITicketSyncDeps::Backend()` return type.
16. `Source_Core/src/OfflineQueueService.cpp:712` — split: `Reader()` for `FetchIssuesForKeys`, `Mutations()` for `UpdateIssueFields`.
17. `Source_Core/include/OfflineQueueService.h` — update `IOfflineQueueDeps` to expose `Reader()` + `Mutations()`.
18. `Source_Core/src/IssueCreatePipeline.cpp:49` — change parameter from `ITrackerClient&` to `ITrackerIssueMutations&`.
19. `Source_Core/src/ProjectResolver.cpp:39` — change parameter from `const ITrackerClient*` to `const ITrackerConnectivity*`.
20. `Source_Core/src/SmatchetProjectPicker.cpp:4` — update to use `ITrackerConnectivity`.
21. `Source_Core/src/SmatchetBulkTicketsUi.cpp:233` — update to use `ITrackerConnectivity`.
22. `Source_Core/src/SmatchetViewsDashboardUi_widgets.cpp:313` — update to use `ITrackerConnectivity`.
23. `Source_Core/src/AppController.cpp:1252` — update to route through `ITrackerBackend` accessors.
24. `Source_Core/src/AppController_CatalogAndFieldEdit.cpp:103` — use `FieldCatalog()` for fetch, `Mutations()` for payload build, `Reader()` for `ResolveDisplayValue`.
25. `Source_Core/src/AppController_Connectivity.cpp:154` — use `Connectivity()` for `ProbeReachability`.
26. `Source_Core/src/AppControllerDepsAdapter.cpp:5` — update adapter to expose narrow types from `ITrackerBackend`.

## Existing utilities reused

- `ITrackerClient` — `Source_Core/include/ITrackerClient.h:58` — becomes the transitional facade (inherits all five interfaces) until call sites are migrated.
- `AppControllerDepsAdapter` — `Source_Core/src/AppControllerDepsAdapter.cpp:5` — already implements `ITicketSyncDeps` + `IOfflineQueueDeps`; updated to expose narrow types.
- `FakeTrackerClient` — `tests/support/FakeTrackerClient.h` — narrowed to test-relevant interfaces, reducing stub count.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no runtime behavior change — pure interface refactor with identical virtual dispatch.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no UI-thread behavior change — same async patterns, just narrower types.
- **Pillar 3 (never crash)**: positive impact — nullptr returns for unsupported capabilities make missing-feature checks explicit; previously, calling a default-stubbed method silently returned an error string.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no UI or accessibility behavior change.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

Diff touches `Source_Core/` headers and call sites but does not change runtime behavior (pure interface refactor — same virtual dispatch, same code paths, same allocations). Perf gates fire but are expected to show zero delta.

1. **PR-fast CI** — fires against `priority-grid-scroll` (exercises the sync → grid → display path through `FetchIssuesStreamed` + `ResolveDisplayValue`).
2. **Pillar 2 static scanner** — N/A — no new sync-I/O paths added.
3. **Dispatcher drain** — N/A — does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — N/A — no new sync-stall code paths.
5. **Marker inventory** — N/A — no `SMATCHET_UI_PERF_SCOPE` markers added.

**Pre-push local check**: run `priority-grid-scroll` scenario before opening the PR.

**Override**: N/A — no regression expected.

## Risks / non-goals

- Multiple inheritance of pure-virtual interfaces is C++14-safe and compilers handle it cleanly, but diamond inheritance must be avoided — no capability interface inherits another. Mitigation: each interface is independent; `ITrackerBackend` composes by accessor, not by inheritance.
- `IssueCreatePipeline` calls 7 distinct `ITrackerIssueMutations` methods in a single flow — if a future refactor splits mutations further (e.g. create vs update), this call site becomes the bottleneck. Accepted — the current 8-method `ITrackerIssueMutations` is already much narrower than the 28-method god interface.
- The facade phase (`ITrackerClient` inheriting all five) adds temporary header coupling — every file that includes `ITrackerClient.h` transitively includes all five interfaces. Mitigation: the facade is deleted in slice 4.
- Fixture backends (`GitHubFixtureBackend`, `PlaneFixtureBackend`) currently override only 8 of 27 methods each. After the split they implement only `ITrackerIssueReader` + `ITrackerIssueMutations`. If a test later needs collaboration features on a fixture, it must add the interface — this is the correct friction.
- Non-goal: change the way backends are instantiated (`BackendFactory::Create()`). The factory continues to return an `ITrackerBackend*` or the facade during transition.
- Non-goal: add runtime capability discovery beyond the nullptr pattern (no `HasCapability(enum)` method).
- Non-goal: split `JiraClient.cpp` / `PlaneClient.cpp` / `GitHubClient.cpp` into per-capability TUs. That's a follow-up file-size concern, not an interface concern.

## Verification

Per `AGENTS.md` verification rules — zero manual steps.

- **Bucket A (pure-logic ctest, `test-rig`)**: existing `SmatchetTests` and `SmatchetLuaTests` must pass unchanged — the refactor is type-level only.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: run `ui-test` scenario — exercises the full grid → detail → edit flow that touches `Reader()` + `Mutations()` + `FieldCatalog()`.
- **Bash-driver scenario / screenshot / sanitizer**: run `scripts/dev/test-all.sh` with MSVC preset. ASAN build via `ninja-msvc-asan` to catch any use-after-free from pointer lifetime changes.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target — confirms both OpenGL and DX12 compile with the new interface hierarchy).
- **Manual residue**: none.

## Out of scope (flagged, not designed)

- Splitting backend implementation files (`JiraClient.cpp` at ~60 KB) into per-capability TUs — follow-up file-size plan if needed after the interface split.
- Adding a Linear backend — follow-up plan (`docs/plans/linear-tracker-backend.md`, since shipped); this refactor made it cheaper but doesn't implement it.
- Runtime capability negotiation UI ("this backend doesn't support comments" banner) — follow-up UX plan.
- Moving `TrackerFieldValueParser` / `TrackerFieldPayload` into `ITrackerIssueMutations` — those are already standalone utilities, not interface methods.

## Implementation log

- `4bc1a17` · Slice 1: extract `ITrackerIssueReader` + `ITrackerConnectivity`; `ITrackerClient` inherits both as facade; `ITicketSyncDeps::Backend()` narrowed to `ITrackerIssueReader*`; `ProjectResolver` narrowed to `ITrackerConnectivity*`; `ITrackerBackend` stub added.

## Deviations from plan

- **`GetTrackerType()` moved to `ITrackerConnectivity` (as planned in the instruction)**: `TicketSyncService` calls `GetTrackerType()` for backend-swap detection. Since `ITicketSyncDeps::Backend()` now returns `ITrackerIssueReader*`, a new `BackendConnectivity()` accessor was added to `ITicketSyncDeps` returning `ITrackerConnectivity*`. This was added to `AppControllerDepsAdapter` (header + impl) and `FakeTicketSyncDeps`. Not a deviation from the intent — the instruction explicitly described this accessor pattern.
- **`AppControllerDepsAdapter.h` now includes `ITrackerClient.h` directly**: covariant return type checking in C++ requires the derived return type (`ITrackerClient*`) to be a complete type at the point of the override declaration. A forward declaration is insufficient. The include was added to the adapter header. This is a minor coupling increase during the transition phase; it disappears when `ITrackerClient` is deleted in Slice 4.
- **`ITrackerIssueReader.h` includes `CachedTicketTypes.h` instead of `LocalCacheManager.h`**: `CachedTicketTypes.h` is the SQLite-free header that defines `CachedTicket`. The plan said to include `LocalCacheManager.h` but the lighter include is strictly better and was already the right call per `CachedTicketTypes.h`'s own header comment.

## Verification (actual)

- **Bucket A**: not run (test suite requires MSVC SDK in PATH; environment missing `stdio.h` for FetchContent curl build — pre-existing issue unrelated to this slice).
- **Build gate (clang preset)**: `cmake --build --preset ninja-iter-clang --target SmatchetStandalone SmatchetCore_DX12` — both targets build clean. Zero errors, only pre-existing `/MP` unused-argument warnings from clang-cl.
- **Build gate (MSVC preset)**: MSVC environment in shell session missing SDK headers (`stdio.h`, `string`, `atomic`); all curl FetchContent TUs fail. Pre-existing issue — confirmed by absence of any `Source_Core/` error lines in MSVC output. The clang build exercises identical `Source_Core/` headers and flags.
- **Covariant return fix**: clang caught `ITrackerClient` incomplete at covariant override point; fixed by adding `#include "ITrackerClient.h"` to `AppControllerDepsAdapter.h`; rebuild was clean.
