# Plan - Deterministic Jira test backend and frontend tests

> **Slug**: `deterministic-jira-test-backend`.
>
> **Mandatory rules cross-link**: see [`AGENTS.md`](../../AGENTS.md) Section Project rules Section Plan location, Section Plan-doc safety, Section Plan revision after implementation, Section Plan stress-test, Section Plan template, Section Plan-doc perf-gate section.
>
> **Grill-with-docs pass**: completed against [`docs/CONTEXT.md`](../CONTEXT.md), the existing tracker/client seams, and the current Bucket E UI-test harness. No ADR is needed: the choice is test-only, reversible, and follows the existing `ITrackerBackendFactory` seam rather than introducing a surprising production architecture decision.

## Context

Smatchet's Jira-facing UI is difficult to test deterministically today. Unit tests already have [`tests/support/FakeTrackerClient.h`](../../tests/support/FakeTrackerClient.h), which is useful for service-level doctests, but it is not a scenario backend the running app can boot with. Bucket E tests under [`tests/ui/`](../../tests/ui/) can drive ImGui interactions, but they either mirror a narrow call-site shape or rely on live app state that is hard to seed without Jira.

After this lands, frontend tests can boot Smatchet in UI-test mode with a scripted Jira-compatible `FakeTrackerClient`, drive real sync/error/edit flows, and assert the grid, warning banners, async sync behavior, and edit affordances without live HTTP, real credentials, or timing flakes.

## Approach

Do not build a separate fake backend from scratch. Reuse [`tests/support/FakeTrackerClient.h`](../../tests/support/FakeTrackerClient.h) as the canonical scripted `ITrackerClient` test double, extend it only where the Jira frontend scenarios need missing virtuals, and add a Jira fixture loader/factory that configures `FakeTrackerClient("Jira")` instances.

Keep fixture drift low by extracting Jira's existing JSON-to-`CachedTicket` mapper from the anonymous namespace in [`Source_Core/src/JiraIssueSearch.cpp`](../../Source_Core/src/JiraIssueSearch.cpp:116) into a small pure helper. The real `JiraClient` and the deterministic fixture loader will share that mapper, so raw Jira search-page fixtures exercise the same normalization path the live backend uses.

Use the fixture-configured fake from two layers: Bucket A doctests validate fixture parsing and scripted call behavior directly against `FakeTrackerClient`, while Bucket E tests launch the app with `SMATCHET_TEST_JIRA_BACKEND_FIXTURE=<path>` and inject a factory that returns freshly configured `FakeTrackerClient("Jira")` instances through the existing `AppController::SetBackendFactory` seam.

## Detailed implementation plan

### Slice 1 - Pure Jira issue mapping extraction

Add `Source_Core/include/JiraIssueMappingPure.h` and `Source_Core/src/JiraIssueMappingPure.cpp`.

Move these helpers out of `JiraIssueSearch.cpp` without behavior changes:

- `BuildFetchFieldListsFromView(const ViewsStore&, vector<string>&, vector<string>&)`
- `JiraAppendCachedTicketFromSearchIssue(...)`, renamed to `AppendCachedTicketFromJiraSearchIssue(...)`

Keep the comment-fetch callback as a `std::function<bool(const std::string&, nlohmann::json&)>` so the production client can still fetch expanded comments, while tests can pass an in-memory callback.

Add `tests/Source_Core/JiraIssueMappingPure.test.cpp` covering:

- basic issue key + selected field mapping
- comments already present vs fetched lazily
- changelog to `history`
- watchers vs watches alias
- missing `fields` object does not throw
- selected custom field object/array stringification

### Slice 2 - Reusable scripted tracker fixture layer

Extend the existing fake rather than adding a second backend implementation.

Planned test-support files:

- [`tests/support/FakeTrackerClient.h`](../../tests/support/FakeTrackerClient.h) - keep as the canonical fake; add missing scripting only for methods required by the planned frontend tests.
- `tests/support/JiraFakeTrackerFixture.h`
- `tests/support/JiraFakeTrackerFixture.cpp`
- `tests/support/ScriptedTrackerBackendFactory.h` if a reusable factory abstraction is cleaner than keeping it in the fixture header.

`JiraFakeTrackerFixture` parses a JSON fixture into an immutable script object. Its factory method returns a fresh `FakeTrackerClient("Jira")` configured from that script. Fresh-per-create matters because `TicketSyncService` can recreate a backend on tracker-type transitions, and test state must not leak across factory calls.

The shared fake should support, either already or after small extensions:

- `ProbeReachability`
- `FetchIssues` / `FetchIssuesStreamed`
- `FetchIssuesForKeys`
- `FetchFieldCatalog`
- `SearchUsersByQuery`
- `FetchIssueEditMeta`
- `UpdateIssueFields`
- `UpdateField`
- `BuildFieldPayload`
- `BuildCreatePayload`
- `CreateIssue`
- `AddIssueCommentPlain`
- call recording for mutation assertions

Fixture JSON shape:

```json
{
  "name": "basic-grid",
  "reachability": {"kind": "AuthenticatedReachable", "diagnostic": ""},
  "catalog": {
    "fields": [],
    "components": [],
    "issueTypeMeta": [],
    "users": []
  },
  "fetches": [
    {
      "fullSyncCompleted": true,
      "warning": "",
      "fetchError": "",
      "selectedFields": ["summary", "status", "priority", "assignee"],
      "jiraSearchPages": [
        {"issues": [], "isLast": true}
      ]
    }
  ],
  "mutations": {
    "updateIssueFields": [{"ok": true}],
    "createIssue": [{"ok": true, "issueKey": "SMAT-99"}]
  }
}
```

Fixture loading rules:

- Prefer `jiraSearchPages` fixtures for sync tests so they flow through `AppendCachedTicketFromJiraSearchIssue`.
- Allow simplified `cachedTickets` only for cases that do not care about Jira JSON normalization.
- Add methods to `FakeTrackerClient` only when a frontend or service test needs them; do not mirror the whole Jira REST surface speculatively.
- Keep all call-recording assertions on `FakeTrackerClient`, so existing doctests and new UI tests use the same inspection model.

### Slice 3 - UI-test startup hook

In [`Target_Standalone/main.cpp`](../../Target_Standalone/main.cpp:524), after constructing `AppController smatchetApp` and before [`smatchetApp.Initialize(...)`](../../Target_Standalone/main.cpp:553), add a `SMATCHET_BUILD_UI_TESTS`-guarded hook:

```cpp
if (const char* fixture = std::getenv("SMATCHET_TEST_JIRA_BACKEND_FIXTURE")) {
    smatchetApp.SetBackendFactory(MakeJiraFakeTrackerBackendFactoryFromFixture(fixture));
}
```

Implementation details:

- The hook is compiled only in UI-test builds.
- Missing/invalid fixture logs an error and falls back to the normal factory only if no fixture env var was set; if the env var is set but invalid, fail fast with a clear test error.
- `tests/ui/CMakeLists.txt` adds `tests/support` to the include path and links only the Jira fixture loader/factory source into `SmatchetStandalone` when `SMATCHET_BUILD_UI_TESTS` is enabled.
- Also compile the fixture loader/factory source into `SmatchetCore_DX12` under the same gate as a no-GL compile tripwire if it stays free of GLFW/OpenGL/ImGui dependencies.

### Slice 4 - Frontend coverage

Add `tests/ui/jira_deterministic_backend.test.cpp` and register it from [`tests/ui/ui_tests_registry.cpp`](../../tests/ui/ui_tests_registry.cpp:28).

Initial Bucket E tests:

- `JiraDeterministicSync_LoadsIssuesIntoGrid`: run `app.SyncWithBackend()` with a fixture-backed Jira client, yield until `!app.IsStreamingSyncActive()`, then assert `GetActiveTickets()` and visible grid text contain `SMAT-1`, summary, status, and priority values.
- `JiraDeterministicSync_TransportErrorKeepsCachedGrid`: first fetch succeeds, second fetch returns a transport-style error. Assert cached rows remain visible and `GetTrackerConnectivityBannerForUi()` reports the cached-data warning.
- `JiraDeterministicSync_PartialWarningDoesNotDeleteStaleRows`: second fetch returns fewer rows with `FullSyncCompleted=false` and a warning. Assert the old row remains and the sync-warning banner is visible.
- `JiraDeterministicSync_SlowBackendDoesNotFreezeFrames`: scripted fetch sleeps on the sync worker. Assert the UI test engine advances frames while `app.IsStreamingSyncActive()` is true and the app does not block the render loop.

Second wave tests after the basic sync surface is stable:

- `JiraDeterministicEdit_SuccessRecordsUpdate`: edit a simple text/select cell, wait for async mutation completion, `dynamic_cast` the active backend from `app.GetTrackerBackendMutable()` to `FakeTrackerClient*`, and assert one recorded `UpdateIssueFields` call with the expected Jira payload.
- `JiraDeterministicEdit_RejectShowsErrorAndKeepsValue`: script the update as a 400-style failure, assert the visible cell does not silently change and the error toast/banner appears.
- `JiraDeterministicCreate_SuccessAddsRow`: use the new-issue draft row against a scripted `CreateIssue` response and assert the new issue key appears after sync/apply.

### Slice 5 - Driver scripts and fixtures

Add fixtures under `tests/fixtures/jira_backend/`:

- `basic-grid.json`
- `transport-error-after-cache.json`
- `partial-warning.json`
- `slow-sync.json`
- `field-edit-success.json`
- `field-edit-reject.json`

Add `scripts/dev/test-ui-jira-deterministic-backend.sh`, mirroring [`scripts/dev/test-ui-views-columns-reorder.sh`](../../scripts/dev/test-ui-views-columns-reorder.sh:1):

- uses `build/ninja-ui-test-msvc/Smatchet.exe` by default
- sets an isolated `SMATCHET_USER_DATA`
- sets `SMATCHET_TEST_JIRA_BACKEND_FIXTURE`
- invokes `Smatchet.exe cmd ui_test.run --name=JiraDeterministic --spawn --yes`
- parses the JSON envelope and returns `0/1/2` like existing dev scripts

## Files to modify

Production/helper files:

1. [`Source_Core/include/JiraIssueMappingPure.h`](../../Source_Core/include/) - new pure mapping API for Jira search issue JSON to `CachedTicket`.
2. [`Source_Core/src/JiraIssueMappingPure.cpp`](../../Source_Core/src/) - new pure implementation extracted from the current Jira search anonymous namespace.
3. [`Source_Core/src/JiraIssueSearch.cpp`](../../Source_Core/src/JiraIssueSearch.cpp:61) - replace anonymous helper bodies with calls to the pure helper.
4. [`Target_Standalone/main.cpp`](../../Target_Standalone/main.cpp:524) - UI-test-only fixture-backed backend-factory hook before `Initialize`.
5. [`CMakeLists.txt`](../../CMakeLists.txt:571) - confirm the new Source_Core helper is picked up by the glob; no special source-list add expected unless the glob cache misses it.

Test support:

6. [`tests/support/FakeTrackerClient.h`](../../tests/support/FakeTrackerClient.h) - extend the existing fake with only the missing scripted methods needed by Jira UI scenarios.
7. [`tests/support/JiraFakeTrackerFixture.h`](../../tests/support/) - new fixture parser/configurator declarations.
8. [`tests/support/JiraFakeTrackerFixture.cpp`](../../tests/support/) - new fixture parser/configurator implementation.
9. [`tests/support/ScriptedTrackerBackendFactory.h`](../../tests/support/) - optional reusable factory wrapper if `JiraFakeTrackerFixture.h` would otherwise carry too much factory code.
10. [`tests/Source_Core/JiraIssueMappingPure.test.cpp`](../../tests/Source_Core/) - new Bucket A mapper tests.
11. [`tests/Source_Core/JiraFakeTrackerFixture.test.cpp`](../../tests/Source_Core/) - new Bucket A fixture-loader and fake-configuration tests.
12. [`tests/CMakeLists.txt`](../../tests/CMakeLists.txt:12) - add new doctest TUs and support source.
13. [`tests/ui/jira_deterministic_backend.test.cpp`](../../tests/ui/) - new Bucket E frontend tests.
14. [`tests/ui/ui_tests_registry.cpp`](../../tests/ui/ui_tests_registry.cpp:28) - register the new UI tests.
15. [`tests/ui/CMakeLists.txt`](../../tests/ui/CMakeLists.txt:13) - include/link Jira fixture support into UI-test builds only.
16. [`tests/fixtures/jira_backend/`](../../tests/fixtures/) - new deterministic Jira fixture set.
17. [`scripts/dev/test-ui-jira-deterministic-backend.sh`](../../scripts/dev/) - new driver script.

## Existing utilities reused

- `AppController::SetBackendFactory` in [`Source_Core/src/AppController.cpp`](../../Source_Core/src/AppController.cpp:352) - existing injection seam for backend factories.
- `AppController::SyncWithBackend` in [`Source_Core/include/AppController.h`](../../Source_Core/include/AppController.h:491) - trigger real streaming sync from UI tests.
- `AppController::GetActiveTickets` and `GetTrackerConnectivityBannerForUi` in [`Source_Core/include/AppController.h`](../../Source_Core/include/AppController.h:515) - stable assertions for backend result and banner state.
- `TicketSyncService::StartStreamingSync` in [`Source_Core/src/TicketSyncService.cpp`](../../Source_Core/src/TicketSyncService.cpp:459) - existing worker-thread path the fixture-configured fake should exercise.
- `FakeTrackerClient` in [`tests/support/FakeTrackerClient.h`](../../tests/support/FakeTrackerClient.h:1) - primary reuse target; keep scripting, call recording, and defaults here instead of cloning them into a Jira-only backend.
- `FakeTrackerBackendFactory` pattern in [`tests/support/FakeTicketSyncDeps.h`](../../tests/support/FakeTicketSyncDeps.h:29) - promote the pattern into a reusable fixture-backed factory rather than inventing a separate app-specific injection style.
- `UiTestScenario` and `SmatchetActiveUiTestAppController` in [`Source_Core/src/Commands/Scenarios/UiTestScenario.cpp`](../../Source_Core/src/Commands/Scenarios/UiTestScenario.cpp:63) - existing in-process UI test bridge.
- Existing UI-test script contract in [`scripts/dev/test-ui-views-columns-reorder.sh`](../../scripts/dev/test-ui-views-columns-reorder.sh:1) - copy the `0/1/2` behavior and JSON parsing shape.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: the fixture-configured fake must not add steady-state production work because it is compiled only in UI-test builds; slow fixtures sleep on the existing sync worker, not the UI thread.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no new sync I/O reaches ImGui render paths. Fixture JSON is loaded before `Initialize`; `FetchIssuesStreamed` runs through `TicketSyncService`'s worker.
- **Pillar 3 (never crash)**: malformed fixture JSON returns a clear test setup error; production Jira mapping extraction gets hostile JSON tests to preserve graceful failure behavior.
- **Pillar 4 (accessibility - keyboard nav / font scaling / WCAG AA)**: no user-facing production UI change. Frontend tests may later assert keyboard-driven edit flows, but this plan does not change accessibility behavior.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A - <reason>`)

1. **PR-fast CI** - fires. Closest existing perf scenario is `priority-grid-scroll` because the new tests seed and render the ticket grid. Behavioral coverage comes from `ui_test.run --name=JiraDeterministic`; perf check should run `bash scripts/dev/perf-run.sh priority-grid-scroll` before PR.
2. **Pillar 2 static scanner** - fires. The scanner should see no new sync I/O reachable from `ImGui::*`; deterministic fixture loading is startup-only and `FetchIssuesStreamed` is worker-threaded.
3. **Dispatcher drain** - N/A. No changes to `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** - N/A for new production code. The slow-backend UI test asserts frames keep advancing rather than adding a new UI-thread blocking path.
5. **Marker inventory** - N/A. No new `SMATCHET_UI_PERF_SCOPE` markers planned.

**Pre-push local check**: run `bash scripts/dev/perf-run.sh priority-grid-scroll` and compare against the dev baseline when available.

## Risks / non-goals

- **Fake backend drifts from real Jira parsing** - mitigated by extracting and sharing `AppendCachedTicketFromJiraSearchIssue`.
- **Test backend leaks into production** - mitigated by `SMATCHET_BUILD_UI_TESTS` guards and by keeping the fixture factory out of `DefaultTrackerBackendFactory`.
- **UI tests become item-label brittle** - mitigate by pairing UI item assertions with app-state assertions and using stable fixture values (`SMAT-1`, unique summaries).
- **Async tests flake** - use frame-yield loops bounded by `IsStreamingSyncActive()` and `GetActiveTicketsRevision()`, not wall-clock sleeps.
- **Fixture schema grows into a second backend product** - keep fixtures scenario-focused; add only methods needed by a frontend test or service regression; reuse `FakeTrackerClient` for behavior instead of growing a Jira-only fake.
- **Real Jira contract regressions still possible** - accepted. This plan adds deterministic frontend coverage, not live Jira E2E. Real-backend smoke remains separate and credential-gated.

Non-goals:

- Add a new production tracker type.
- Mock `cpr` globally.
- Cover Plane or GitHub in this first plan.
- Regenerate or approve any golden image.
- Add live Jira credential tests to CI.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**:
  - `JiraIssueMappingPure.test.cpp`
  - `JiraFakeTrackerFixture.test.cpp`
  - targeted run: `ctest --test-dir build/ninja-test-msvc --output-on-failure -R "smatchet_tests"`
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**:
  - `tests/ui/jira_deterministic_backend.test.cpp`
  - targeted driver: `bash scripts/dev/test-ui-jira-deterministic-backend.sh`
- **Bash-driver scenario / screenshot / sanitizer**:
  - new bash driver uses isolated `SMATCHET_USER_DATA`
  - no screenshot/golden output
  - sanitizer remains the standard `ninja-test-msvc` gate for Source_Core changes
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Manual residue**: none expected. If a stable grid cell cannot be asserted through ImGui Test Engine, add a `docs/self-improvement/categories/tooling.md` entry and keep the app-state assertion as the temporary automated guard.

## Out of scope (flagged, not designed)

- **Plane/GitHub fixture-backed fakes** - follow the same fixture/factory shape once the Jira path proves stable.
- **Live Jira smoke** - separate credential-gated script, not CI.
- **Full field-editor matrix** - start with text/select success and failure; rich text/date/sprint edits get follow-up tests.
- **Contract tests against Atlassian's live OpenAPI schema** - useful later, but too broad for the frontend determinism goal.

## Implementation log

- `9a7100a6` — Slice 1: extract `JiraIssueMappingPure` (`.h` + `.cpp`) from `JiraIssueSearch.cpp` anon namespace; 14 Bucket A tests; dual-target clean
- `3eaf15d4` — Slice 2: `JiraFakeTrackerFixture` + `ScriptedTrackerBackendFactory` + `FakeTrackerClient::EnqueueFetchResult`; 10 Bucket A fixture tests (959 total)
- `bd163c2d` — Slice 3: `SMATCHET_BUILD_UI_TESTS` fixture-injection hook in `StandaloneAppBootstrap::InitAppAndPlugins`; `tests/ui/CMakeLists.txt` wires include path + `JiraFakeTrackerFixture.cpp` into UI-test builds
- `4cb7a0a5` — Slices 4+5: 3 Bucket E tests (`jira_deterministic_backend.test.cpp`), 5 fixture JSON files under `tests/fixtures/jira_backend/`, `test-ui-jira-deterministic-backend.sh` driver

## Deviations from plan

- `JiraFakeTrackerFixture` is split `.h`/`.cpp` in `tests/support/` as planned, but the `.cpp` is also linked into `SmatchetStandalone` under `SMATCHET_BUILD_UI_TESTS` (via `tests/ui/CMakeLists.txt`), not `tests/ui/CMakeLists.txt` alone — same effect, cleaner than a separate target.
- `ScriptedTrackerBackendFactory` owns `JiraFakeTrackerFixture` by value (not raw pointer) to avoid the lifetime hazard when `AppController` holds the factory.
- The hook lives in `StandaloneAppBootstrap.cpp::InitAppAndPlugins` (not `main.cpp`) because `AppController` is constructed there, not in `main`.
- `slow-sync.json` fixture not created (no `sleepMs` mechanism in `FakeTrackerClient`); the slow-backend Bucket E test uses the basic-grid fixture and asserts frames advance — still exercises the non-blocking invariant.
- Second-wave edit/create Bucket E tests (`JiraDeterministicEdit_*`, `JiraDeterministicCreate_*`) deferred to a follow-up — the basic sync surface must be stable under real UI-test runs first.

## Verification (actual)

- **Bucket A (`ninja-test-msvc`, ctest)**: 959 tests pass (949 pre-existing + 14 `JiraIssueMappingPure` + 10 `JiraFakeTrackerFixture`). `ctest --test-dir build/ninja-test-msvc --output-on-failure -R smatchet_tests` → `Passed 3.29s`.
- **Bucket E compile gate**: `cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone` → exit 0, no warnings in new TUs.
- **Bucket E runtime**: not run — requires `SMATCHET_TEST_JIRA_BACKEND_FIXTURE` set and live GLFW window. Driver: `bash scripts/dev/test-ui-jira-deterministic-backend.sh`.
- **Dual-target**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` → exit 0.
