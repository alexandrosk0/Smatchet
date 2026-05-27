# Plan - Linear as fourth tracker backend

> **Slug**: `linear-tracker-backend` (matches this file's basename without `.md`).
>
> **Origin**: User request, 2026-05-26: "Can I add Linear as a tracker to smatchet?" followed by "make a plan for it".
>
> **Mandatory rules cross-link**: see `AGENTS.md` Project rules for plan location, plan-doc safety, plan revision after implementation, plan stress-test, plan template, and plan-doc perf-gate section.

## Context

Smatchet already has a tracker abstraction (`ITrackerClient`) and three in-tree tracker backends: Jira, Plane, and GitHub. Linear fits this architecture as a fourth backend because its issue model maps cleanly onto Smatchet's grid concepts: issue identifier, title, description, workflow state, assignee, labels, priority, project, cycle, timestamps, comments, create, and update.

Linear's public API is GraphQL at `https://api.linear.app/graphql`, supports personal API keys and OAuth2, and returns GraphQL errors in a normal `errors` array even when HTTP status is 200. The MVP should use personal API keys, not OAuth, matching Smatchet's existing local-user tracker profile model. References: Linear GraphQL getting started (`https://linear.app/developers/graphql?noRedirect=1`) and Linear rate limiting (`https://linear.app/developers/rate-limiting`).

After this lands, selecting `Linear` in Preferences lets a user sync Linear issues into the existing Smatchet grid, edit core fields, create issues, and add plain comments without special-case UI outside the tracker-specific config panel.

## Approach

Implement Linear as a normal `ITrackerClient` backend, mirroring the GitHub rollout rather than introducing a parallel abstraction. Keep the first production slice read-only so the GraphQL transport, pagination, field mapping, view defaults, and Preferences switch can stabilize before mutations are enabled.

Use `cpr` plus `nlohmann::json` directly. Do not add a Linear SDK dependency: the project already has the HTTP and JSON stack, and a TypeScript SDK does not belong in the C++14 core. Build a tiny GraphQL helper that posts `{ "query": "...", "variables": { ... } }`, checks HTTP status, checks the `errors` array, and exposes rate-limit headers in diagnostics.

Ship in four slices:

1. **Substrate + read-only profile**: config fields, Preferences UI, factory/sync switch, default Linear view, GraphQL probe, `ListProjects()` as Linear teams, field catalog for one team, and bucket-A tests.
2. **Read sync + query translation**: paginated issue fetch, `FetchIssuesForKeys`, pure JSON-to-`CachedTicket` mapping, and a limited `LinearQueryFromJql` helper for Smatchet's existing view query box.
3. **Writes + creation + comments**: `issueUpdate`, `issueCreate`, `commentCreate`, payload building, create-pipeline integration, audit trail, and offline replay compatibility.
4. **Polish + coverage**: ImGui Test Engine coverage for Preferences tracker switch, rate-limit diagnostics, live-smoke manual residue documentation, and any backlog entries for fields intentionally left read-only.

Key decisions:

- Linear team is the Smatchet draft scope for the first version. Internally this rides through existing `ProjectKey` / `RemoteProject` plumbing, but user-facing Linear UI should say `Team` so it does not collide with Linear's separate first-class `Project` issue field.
- Store `LinearApiUrl` with default `https://api.linear.app/graphql` for tests and future compatibility, but keep the visible UI compact: API key, team, and optional workspace URL/display hint.
- Keep the view editor's existing JQL-shaped text box for now. Translate a small subset (`assignee=currentUser()`, `status`, `labels`, `priority`, `project`, text search, AND/OR where practical) into Linear GraphQL filter variables. Unsupported clauses produce a soft warning and are ignored, matching the GitHub translator pattern.
- Request Linear's issue `url` when available and persist it as a field value for browse links. If the API response does not include a URL, `BuildBrowseUrl` returns empty rather than guessing a broken workspace URL.
- Use option IDs, not display labels, for mutable select-like fields. Field catalog options should carry Linear UUIDs in `TrackerFieldOption::Id` and display text in `Value`.

## Files to modify

Core backend additions:

1. `Source_Core/include/LinearClient.h`: new `ITrackerClient` implementation declaration.
2. `Source_Core/src/LinearClient.cpp`: tracker shell, reachability probe, field catalog, browse URL, and routing for shared helpers.
3. `Source_Core/include/LinearClientHelpers.h` and `Source_Core/src/LinearClientHelpers.cpp`: GraphQL headers, API URL normalization, issue key parsing, ISO timestamp parsing, and GraphQL error extraction.
4. `Source_Core/src/LinearIssueSearch.h` and `Source_Core/src/LinearIssueSearch.cpp`: paginated issue fetch and per-key lookup.
5. `Source_Core/include/LinearIssueMappingPure.h` and `Source_Core/src/LinearIssueMappingPure.cpp`: pure JSON issue node to `CachedTicket` mapping.
6. `Source_Core/include/LinearQueryFromJql.h` and `Source_Core/src/LinearQueryFromJql.cpp`: pure translator from Smatchet view query text to Linear GraphQL filter/search variables.
7. `Source_Core/src/LinearIssueMutation.cpp`: `issueUpdate`, `issueCreate`, and `commentCreate` implementation split out of the client shell.

Config and UI:

8. [`Source_Core/include/ConfigManager.h`](../../Source_Core/include/ConfigManager.h:60): add `LinearApiKey`, `LinearApiUrl`, `LinearTeamId`, `LinearTeamKey`, `LinearWorkspaceUrl`, and `NewIssueInheritFieldIdsLinear`.
9. [`Source_Core/src/ConfigManager.cpp`](../../Source_Core/src/ConfigManager.cpp:381): persist Linear config; DPAPI-encrypt `LinearApiKey`; map `SMATCHET_TRACKER_TOKEN` and `SMATCHET_TRACKER_BASE_URL` for `TrackerType=Linear`.
10. [`Source_Core/src/ConfigManager_Views.cpp`](../../Source_Core/src/ConfigManager_Views.cpp:131): add Linear default view and `NormalizeViewsBackendKey("linear") -> "Linear"`.
11. [`Source_Core/include/SmatchetUiSession.h`](../../Source_Core/include/SmatchetUiSession.h:293): add Preferences buffers for Linear fields.
12. [`Source_Core/src/SmatchetPreferencesUi.cpp`](../../Source_Core/src/SmatchetPreferencesUi.cpp:292): add `Linear` to the tracker combo, render the Linear config panel, save/load buffers, and filter recently used projects by the Linear endpoint.
13. [`Source_Core/src/FieldCatalogCache.cpp`](../../Source_Core/src/FieldCatalogCache.cpp:433): add a Linear cache key shape, likely `Linear|<api-url>|<team-id-or-key>`.
14. [`Source_Core/include/FieldCatalogCache.h`](../../Source_Core/include/FieldCatalogCache.h:14): update comments/schema docs for the Linear cache key.

Factory, sync, and shared flow:

15. [`Source_Core/src/DefaultTrackerBackendFactory.cpp`](../../Source_Core/src/DefaultTrackerBackendFactory.cpp:9): include `LinearClient.h` and construct `LinearClient` for `trackerType=Linear`.
16. [`Source_Core/src/TicketSyncService.cpp`](../../Source_Core/src/TicketSyncService.cpp:443): add Linear backend switching and stale-grid clearing parity with the other backends.
17. [`Source_Core/src/ProjectResolver.cpp`](../../Source_Core/src/ProjectResolver.cpp:33): resolve Linear team from explicit draft state or a supported `team = ...` query clause; do not treat Linear issue `project` as the draft scope.
18. [`Source_Core/src/SmatchetNewIssueDraftUi.cpp`](../../Source_Core/src/SmatchetNewIssueDraftUi.cpp:439): render the draft scope picker as `Team` when the active backend is Linear, while leaving Jira/Plane wording unchanged.
19. [`Source_Core/include/NewIssueInheritDefaults.h`](../../Source_Core/include/NewIssueInheritDefaults.h:6): verify default inherit fields make sense for Linear; add a Linear-specific list only if the shared default is wrong.
20. [`Source_Core/include/IssueCreatePipeline.h`](../../Source_Core/include/IssueCreatePipeline.h:29): comments may need neutral wording where they still say "Jira payload" for a now-four-backend path.

Tests and build:

21. [`tests/CMakeLists.txt`](../../tests/CMakeLists.txt:98): add new pure helper tests and any non-network client tests.
22. `tests/Source_Core/LinearClientHelpers.test.cpp`: GraphQL error handling, API URL normalization, and issue key parsing.
23. `tests/Source_Core/LinearIssueMappingPure.test.cpp`: JSON fixture to `CachedTicket` mapping, including nulls and missing optional relations.
24. `tests/Source_Core/LinearQueryFromJql.test.cpp`: supported/unsupported query translation and warning behavior.
25. `tests/Source_Core/ConfigManager.test.cpp`: Linear config round-trip, secret migration shape, and env overrides.
26. `tests/Source_Core/TicketSyncService.test.cpp`: switching Jira/Plane/GitHub/Linear clears active tickets and recreates the backend.
27. `tests/ui/*preferences*tracker*.test.cpp` or a new scenario: Preferences tracker combo includes Linear and switching to Linear renders the right fields.
28. `tests/fixtures/linear/*.json`: deterministic GraphQL responses for teams, issues, field catalog, and mutation success/error bodies.

## Existing utilities reused

- [`ITrackerClient`](../../Source_Core/include/ITrackerClient.h:58): existing tracker abstraction; Linear implements the same virtuals as Jira, Plane, and GitHub.
- [`DefaultTrackerBackendFactory::Create`](../../Source_Core/src/DefaultTrackerBackendFactory.cpp:9): one construction point for backend selection.
- [`TicketSyncService::StartStreamingSync`](../../Source_Core/src/TicketSyncService.cpp:443): existing async worker path; all Linear network I/O stays off the UI thread.
- [`TrackerPostLogged`](../../Source_Core/src/TrackerHttpUtils.cpp:139): GraphQL is POST-only, so reuse tracker HTTP logging/timeouts instead of adding ad hoc `cpr::Post` calls.
- [`IsTrackerTransportErrorText`](../../Source_Core/src/TrackerHttpUtils.cpp:161): reuse sync warning/connectivity classification.
- [`ConfigManager` secret persistence](../../Source_Core/src/ConfigManager.cpp:381): same DPAPI + plaintext legacy fallback pattern as Plane API key and GitHub PAT.
- [`ConfigManager::NormalizeViewsBackendKey`](../../Source_Core/src/ConfigManager_Views.cpp:217): extend existing per-backend view buckets.
- [`FieldCatalogCache`](../../Source_Core/src/FieldCatalogCache.cpp:433): cache Linear field catalog snapshots by backend/endpoint/team.
- [`IssueCreatePipeline`](../../Source_Core/include/IssueCreatePipeline.h:29): existing create/update validation and offline queue integration.
- [`FakeTrackerClient`](../../tests/support/FakeTrackerClient.h:69): test sync switching and create/update integration without live Linear credentials.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: Linear issue mapping and cache writes happen in the existing background sync/apply budget; no per-frame GraphQL work.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: all Linear GraphQL calls run through `TicketSyncService` workers or existing create/update/offline flows; Preferences save may probe only through the existing visible save/probe path.
- **Pillar 3 (never crash)**: GraphQL null-heavy responses are mapped defensively in pure helpers; 200-with-errors is treated as failure or partial warning, never as a valid full sync.
- **Pillar 4 (accessibility - keyboard nav / font scaling / WCAG AA)**: Preferences uses existing ImGui controls and layout; add bucket-E coverage for the tracker combo so the new fields remain reachable.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A - <reason>`)

1. **PR-fast CI** - fires. No Linear-specific perf scenario exists today; run `idle` for config/factory/worker reachability and `priority-grid-scroll` once mapped Linear tickets enter the grid. If a `tracker_sync` scenario is added before implementation, switch to that named scenario.
2. **Pillar 2 static scanner** - fires. Any new GraphQL helper callable from sync/update paths gets reviewed for UI-thread reachability; worker-only calls should carry `/* PILLAR2_WORKER_ONLY */ // est-latency: 15000ms` near the boundary.
3. **Dispatcher drain** - N/A. The plan does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** - fires if Preferences save adds a live Linear probe or any other sync stall. Use existing sync toast/progress affordances; add/extend a UI test if a new stall path appears.
5. **Marker inventory** - N/A. No new `SMATCHET_UI_PERF_SCOPE` markers planned.

**Pre-push local check**: `bash scripts/dev/perf-run.sh idle priority-grid-scroll`, plus any Linear-specific scenario added during implementation.

**Override**: `perf-out-of-band` PR label only for an intentional regression with a queued baseline/follow-up.

## Risks / non-goals

- **GraphQL 200-with-errors**: easy to misclassify as success. Mitigation: helper checks `errors` before reading `data`; partial data becomes a warning only when the requested page is usable.
- **Linear filter semantics differ from JQL**: full JQL parity is unrealistic. Mitigation: limited translator with warnings, tests, and no silent broadening beyond safe clauses.
- **Linear team vs Linear project terminology**: Smatchet currently calls the draft scope `ProjectKey`. Mitigation: keep that as internal plumbing only; Linear UI and docs call it `Team`, and Linear `project` remains a mutable issue field.
- **Option display names are not stable IDs**: states, labels, users, projects, and cycles need UUIDs. Mitigation: field catalog stores UUIDs in `TrackerFieldOption::Id` and uses display names only for UI.
- **Pagination and rate limits**: Linear is rate-limited and cursor-paginated. Mitigation: cap pages, surface a soft warning at cap, and log/use rate-limit headers in diagnostics.
- **Field catalog can grow large on big teams**: users, labels, and projects may be numerous. Mitigation: cap/search where Linear supports it; keep initial catalog to fields needed by create/update.

## Non-goals

- **Linear OAuth app flow**: future plan if Smatchet needs shared/team installs rather than local personal tokens. Personal API key only for the first version.
- **Webhook-driven incremental sync**: future plan; not needed for parity with Jira/Plane polling.
- **Attachment upload/download**: future plan because Linear's file storage auth is its own flow.
- **Linear customer objects, initiatives, documents, relations, estimates, and roadmap-specific UX**: out of scope for the tracker surface.
- **Full GraphQL schema introspection at runtime**: hand-written queries are smaller and adequate for the narrow tracker surface.
- **Linear-native query UI**: follow-up only if the JQL-subset translator proves too limiting.
- **Multi-team aggregation**: single active team first; multi-team views need a separate UX decision and more pagination/rate-limit work.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `LinearClientHelpers`, `LinearIssueMappingPure`, `LinearQueryFromJql`, config round-trip/env override, field catalog option mapping, and GraphQL error parser tests.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: Preferences tracker switch scenario verifies Linear appears, config buffers render, save persists the Linear profile, and backend-kind switching clears old active tickets.
- **Bash-driver scenario / screenshot / sanitizer**: no screenshot/golden expected. Run sanitizer build after mutation slice because payload building touches user-entered JSON-like data and offline replay.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Manual residue**: one live Linear smoke remains unavoidable unless CI has test credentials: configure a personal API key, fetch one team, sync issues, update a throwaway issue title/label, add a comment, and create a test issue. Track automation follow-up in `docs/backlog/agent-self-improvement/tooling.md` if this remains manual at ship time.

## Implementation log

*(populated post-ship per `AGENTS.md` plan revision after implementation - bullet per shipped commit: `<sha> - <one-line summary>`)*.

## Deviations from plan

*(populated post-ship - what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*.

## Verification (actual)

*(populated post-ship - what was actually tested plus result: passed / failed / not-run)*.
