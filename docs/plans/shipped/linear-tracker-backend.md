# Plan - Linear as fourth tracker backend

> **Slug**: `linear-tracker-backend` (matches this file's basename without `.md`).
>
> **Status**: `shipped` (2026-06-23 status reconciliation) — the "not started / no production code shipped yet" line was **stale**. The full backend (slices 1–4) landed via **#1453**; live-smoke automation + bucket-E red-fix followed in **#1486 / #1489 / #1491**. 16 `Linear*.{h,cpp}` TUs + 4 Bucket-A tests + fixtures + the `linear-live-smoke.yml` workflow are on develop, and the post-ship sections (§ Implementation log / Deviations / Verification) below are populated. The plan doc originally merged in #466; **revised 2026-06-20** (#1448) to the current tracker architecture (`ITrackerBackend` + 6 role interfaces, post `tracker-interface-split`) and the latest Linear GraphQL API. Only residue: the manual live-smoke credential step (§ Verification).
>
> **Origin**: User request, 2026-05-26: "Can I add Linear as a tracker to smatchet?" → "make a plan for it". Revision request, 2026-06-20: "this plan is very old, start over from scratch and revise the plan to be following the latest Linear api and the latest code features and structure."
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules + [`process-rules.md`](../../agent-rules/process-rules.md) § Plan-doc family for plan location, plan-doc safety, plan revision after implementation, the `grill-with-docs` stress-test (run before implementation starts), and the plan-doc perf-gate section.
>
> **Parallel-work coordination (2026-06-20)**: the `appcontroller-god-object-decomposition` plan (#1447) is being implemented concurrently — a 6-phase, behavior-preserving refactor that churns `AppController.{h,cpp}`, `GridContextDepsAdapter.*`, and the `Initialize` service-wiring, extracting services behind `*Deps` interfaces. **Impact here**: the `AppController.cpp` line anchors in § Files to modify (fixture-install hook ~L1899, factory instantiation ~L1824) are a moving target — **re-verify against HEAD at implementation time**, and expect that wiring may have relocated into an extracted service. Land the Linear *substrate* slice (factory `linear` arm + config + sync-service swap — none of which touch AppController internals) independently of the decomposition; do the `AppController.cpp` fixture-install hook (Bucket-E only, slice 4) last, rebased on the latest develop. Overlap is otherwise low — Linear is constructed via `DefaultTrackerBackendFactory`, not via the deps adapter the decomposition extends.

## Context

Smatchet has a tracker abstraction and **three** in-tree backends: Jira, Plane, and GitHub. Since the original plan, the single `ITrackerClient` was split (shipped `tracker-interface-split`) into a thin facade **`ITrackerBackend`** plus **six role interfaces** — `ITrackerIssueReader`, `ITrackerConnectivity`, `ITrackerFieldCatalog`, `ITrackerIssueMutations`, `ITrackerCollaboration`, `ITrackerActivity` (the 6th added per ADR-0021). A backend multiply-inherits the facade + every role it supports and self-returns from the role accessors. **`GitHubClient` is now the closest analog** Linear should mirror, including its file decomposition (thin HTTP adapter + cpr-free pure TUs that the doctest rig can link).

Linear fits this architecture as a fourth backend: its issue model maps cleanly onto Smatchet's grid (identifier, title, description, workflow state, assignee, labels, priority, project, cycle, timestamps, comments, create, update). Linear's public API is **GraphQL at `https://api.linear.app/graphql`** (POST only). The MVP authenticates with a **personal API key** (matching Smatchet's local-user tracker-profile model), not OAuth.

**Latest Linear API facts baked into this revision** (verified 2026-06-20 — the schema is large, unversioned, and uses `@deprecated` directives; no structural change to the issue model):

- **Auth**: personal API key is sent in the `Authorization: <key>` header **with no `Bearer` prefix** (this differs from GitHub's `Authorization: Bearer <pat>` — easy to get wrong). OAuth2 access tokens *do* use `Bearer`, but OAuth is a non-goal here.
- **Errors**: GraphQL returns errors in a top-level `errors` array, frequently with **HTTP 200** even on failure. The transport helper must check `errors` before reading `data`.
- **Rate limiting**: API-key auth allows **5,000 requests/hour** (per *user*, shared across that user's keys) plus a **complexity budget of 3,000,000 points/hour**; some endpoints carry lower individual limits. Each response carries lowercase headers `x-ratelimit-requests-{limit,remaining,reset}`, `x-ratelimit-complexity-{limit,remaining}`, and `x-complexity`. Surface these in diagnostics.
- **Issue fields**: `id` (UUID), `identifier` (e.g. `ENG-123`), `number`, `title`, `description` (markdown), `url`, `priority` (Int **0–4**: 0 None, 1 Urgent, 2 High, 3 Medium, 4 Low) + `priorityLabel`, `state { id name type color }` (type ∈ triage/backlog/unstarted/started/completed/canceled), `assignee { id name displayName email }`, `creator`, `labels { nodes { id name color } }`, `team { id key name }`, `project { id name }`, `cycle { id number name }`, `estimate`, `createdAt`, `updatedAt`, `completedAt`, `dueDate`, `comments { nodes { id body user { displayName } createdAt } }`.
- **Pagination**: cursor-based — `issues(first: N, after: $cursor, filter: {...}) { nodes { … } pageInfo { hasNextPage endCursor } }` (default page 50).
- **Filtering** (`IssueFilter`): comparators `eq/neq/in/nin/lt/lte/gt/gte/contains/containsIgnoreCase/startsWith/null`; filterable by `assignee`, `state`, `labels`, `priority`, `project`, `team`, `title`, `createdAt`, `updatedAt`, plus `and`/`or`. Current user is `assignee: { isMe: { eq: true } }`.
- **Mutations** return a payload object `{ success, <entity> }`: `issueCreate(input: IssueCreateInput!)` (input `teamId` required, plus `title/description/priority/stateId/assigneeId/labelIds/projectId/cycleId/parentId/estimate/dueDate`), `issueUpdate(id, input: IssueUpdateInput!)` (all optional), `commentCreate(input: CommentCreateInput!)` (`issueId`, `body`).
- **Teams** are the Smatchet "Team" draft scope: `teams { nodes { id key name } }`; per-team catalog via `team(id) { states {…} labels {…} members {…} projects {…} cycles {…} }`.

After this lands, selecting `Linear` in Preferences lets a user sync Linear issues into the existing Smatchet grid, edit core fields, create issues, and add plain comments — no special-case UI outside the tracker-specific config panel.

## Approach

Implement Linear as a normal `ITrackerBackend` (multiply-inheriting the facade + the 6 roles, exactly like `GitHubClient`), not a parallel abstraction. Keep the first production slice **read-only** so the GraphQL transport, pagination, field mapping, view defaults, and the Preferences switch stabilize before mutations are enabled.

Use `cpr` + `nlohmann::json` directly — **no Linear SDK** (the TS SDK does not belong in the C++14 core; the HTTP/JSON stack already exists). Route every GraphQL POST through the shared **`TrackerPostLogged`** helper (logging, timeouts, retry, redirect-disabled SSL, network-usage accounting) rather than ad-hoc `cpr::Post`. Build a tiny GraphQL helper that posts `{ "query": "...", "variables": { ... } }`, checks HTTP status, checks the `errors` array, and exposes rate-limit headers in diagnostics.

**Mirror GitHub's file decomposition**: keep the HTTP-touching adapter thin and split all pure logic (key parsing, JSON→`CachedTicket` mapping, query translation, payload building, error/header parsing) into cpr-free / SQLite-free / ImGui-free TUs in `namespace smatchet::linear`, so the Bucket-A doctest rig links them without a network stack. Override `FetchIssuesStreamed` for per-page streaming (the sync worker calls it directly).

Ship in four slices:

1. **Substrate + read-only profile**: config fields + secret persistence + env routing; backend-key registry (`KnownBackendKeys`/`NormalizeViewsBackendKey`/`BackendCredentialsPresent`) + default Linear view; Preferences combo + Linear config panel + session buffers; factory `linear` arm; sync-service backend-swap branch; `LinearClient` shell (facade + 6 roles); GraphQL transport + `ProbeReachability` + `GetTrackerType`; `ListProjects()` = Linear teams; `FieldCatalogCache` key; field catalog for one team; Bucket-A tests.
2. **Read sync + query translation**: paginated `issues` fetch (`FetchIssues` / `FetchIssuesStreamed` override / `FetchIssuesForKeys`); pure JSON-node → `CachedTicket` mapping; `LinearQueryFromJql` (limited JQL-subset → `IssueFilter` variables) for the existing view query box; `BuildBrowseUrl` from the issue `url`.
3. **Writes + creation + comments**: `issueUpdate` (`UpdateField` set-replace, `UpdateIssueFields`, `BuildFieldPayload`), `issueCreate` (`BuildCreatePayload`, `CreateIssue`), `commentCreate` (`AddIssueCommentPlain`); option-ID payload building; `IssueCreatePipeline` integration (via the mutations role — the pipeline is backend-agnostic, no edits there); offline-replay compatibility; new-issue draft "Team" wording; sanitizer build.
4. **Polish + coverage**: ImGui Test Engine Bucket-E (Preferences tracker switch + a `LinearFixtureBackend` deterministic replay), rate-limit diagnostics, live-smoke manual-residue doc, backlog entries for fields intentionally left read-only.

Key decisions:

- **`GetTrackerType()` returns `"Linear"` (PascalCase)** to match `KnownBackendKeys()` and the Preferences combo item, *avoiding* the GitHub divergence where the client reports lowercase `"github"` while the canonical key is `"GitHub"`. The factory `Create()` and the sync swap both lowercase before comparing (safe either way), but the **`ConfigManager` env-override block compares raw `cfg.TrackerType`** — so the Linear arms there must compare case-insensitively (and this revision flags the existing GitHub lowercase `"github"` env compare as a latent casing trap to verify, not copy).
- **Linear *team* is the Smatchet draft scope** for v1. Internally it rides existing `ProjectKey`/`RemoteProject` plumbing, but user-facing Linear UI says **`Team`** so it doesn't collide with Linear's separate first-class `project` issue field.
- Store `LinearBaseUrl` (default `https://api.linear.app/graphql`) mirroring `GitHubBaseUrl`; keep the visible UI compact (API key, team, optional workspace URL/display hint).
- Keep the existing JQL-shaped view text box; translate a small safe subset (`assignee=currentUser()` → `isMe`, `status`, `labels`, `priority`, `project`, `team`, text search, AND/OR where practical) into Linear `IssueFilter` variables. Unsupported clauses → soft warning + ignored (the GitHub-translator pattern).
- Persist Linear's issue `url` as a field value; `BuildBrowseUrl` returns it, or empty rather than guessing a broken workspace URL.
- Use **option IDs (UUIDs), not display labels**, for mutable select-like fields: `TrackerFieldOption::Id` carries the Linear UUID, `::Value` the display text. Priority is the fixed 0–4 enum (no fetch).

## Files to modify

> Paths/line-anchors are current as of 2026-06-20; treat lines as approximate. **CMake auto-globs `Source/Core/src/**.cpp` with `CONFIGURE_DEPENDS`, so new `Source/Core/.../Linear*.cpp` need no root-CMake edit** — only `tests/CMakeLists.txt` registration is manual.

**New core backend files** (headers in `Source/Core/include/Tracker/`, sources in `Source/Core/src/Tracker/`; the cpr-free pure TUs are the ones the doctest rig links):

1. `include/Tracker/LinearClient.h` + `src/Tracker/LinearClient.cpp`: `class LinearClient : public ITrackerBackend, public ITrackerIssueReader, public ITrackerConnectivity, public ITrackerFieldCatalog, public ITrackerIssueMutations, public ITrackerCollaboration` (Activity role optional — skip in MVP). Ctor `LinearClient(std::string baseUrl, std::string apiKey)`, role accessors returning `*this`/`this`, per-request `ResolveAuth(const TrackerConfig*)` (never latch ctor creds — the #979 pattern), `GetTrackerType()`→`"Linear"`, `ProbeReachability`, `ListProjects()` = Linear teams, `FetchFieldCatalog`, `BuildBrowseUrl`, `BuildLinearHeaders` (Authorization = raw key, no Bearer; `Content-Type: application/json`).
2. `include/Tracker/LinearClientHelpers.h` + `src/Tracker/LinearClientHelpers.cpp` (pure, `namespace smatchet::linear`): GraphQL request-body builder, API-URL normalization/validation, identifier (`ENG-123`) parse/format, ISO-8601→unix-sec parse, GraphQL `errors`-array extraction, rate-limit header parse, `ResolveLinearRequestAuth`.
3. `src/Tracker/LinearIssueSearch.h` + `src/Tracker/LinearIssueSearch.cpp` (private, next-to-impl, HTTP-touching): cursor-paginated `issues` fetch behind `FetchIssues`/`FetchIssuesStreamed`/`FetchIssuesForKeys`; declares the shared `BuildLinearHeaders`.
4. `include/Tracker/LinearIssueMappingPure.h` + `src/Tracker/LinearIssueMappingPure.cpp` (pure): JSON issue node → `CachedTicket` (null-safe; missing optional relations).
5. `include/Tracker/LinearQueryFromJql.h` + `src/Tracker/LinearQueryFromJql.cpp` (pure): JQL-subset → Linear `IssueFilter`/search variables + warning list (mirrors `GitHubQueryFromJql`).
6. `src/Tracker/LinearIssueMutation.cpp`: `issueUpdate`/`issueCreate`/`commentCreate` + payload building, split out of the client shell.
7. `include/Tracker/LinearFixtureBackend.h` + `src/Tracker/LinearFixtureBackend.cpp`: read-only fixture backend (canned JSON; writes logged no-ops) for Bucket-E, mirroring `GitHubFixtureBackend`.

**Config + UI (modified)**:

8. `include/Config/ConfigManager.h` (`struct TrackerConfig`, ~L62–209): add `LinearApiKey`, `LinearBaseUrl` (default `https://api.linear.app/graphql`), `LinearTeamId`, `LinearTeamKey`, `LinearWorkspaceUrl`, and `NewIssueInheritFieldIdsLinear` — mirroring the `GitHubPat/GitHubBaseUrl/GitHubOwner/GitHubRepo` block.
9. `src/Config/ConfigManager.cpp`: persist Linear config; DPAPI-encrypt `LinearApiKey` in all three save arms (`linear_api_key_enc` + Android `sealSecret` + non-Win32 plaintext) and read it in `LoadSecretFields` (`*_enc` + legacy plaintext fallback) with a `PurgeLegacyAgenticKeys` entry; add Linear arms to the `SMATCHET_TRACKER_TOKEN` (~L1379) and `SMATCHET_TRACKER_BASE_URL` (~L1392) routing chains — **case-insensitive compare** (see casing decision above).
10. `src/Config/ConfigManager_Views.cpp`: add `"linear"→"Linear"` to `NormalizeViewsBackendKey` (~L226) **and** `"Linear"` to `KnownBackendKeys()` (~L259) — a CI guard (`ConfigManagerViews.test.cpp:80`) fails if these two disagree; add a Linear branch to `MakeDefaultViewWorkspaceForBackend` (~L142, default view + fields) and to `BackendCredentialsPresent` (~L264, require `LinearApiKey + LinearTeamId`).
11. `include/Ui/SmatchetUiSession.h` (~L436–452): add `linearApiKeyBuf`/`linearBaseUrlBuf`/`linearTeamBuf`/`linearWorkspaceBuf`/`newIssueInheritFieldsLinearBuf` alongside the GitHub buffers.
12. `src/Ui/SmatchetPreferencesUi.cpp`: append `"Linear"` to the combo `items[]` (~L238) + index branch; add a Linear config panel (currently GitHub is the `else` at ~L311 — make Linear an explicit `currentItem == 3` branch); seed buffers in `loadPreferencesBuffers` (~L182) and write them back in `onPreferencesSaveAndSync` (~L584, incl. inherit-CSV + per-backend log line).
13. `src/Tracker/FieldCatalogCache.cpp` (~L431) + `include/Tracker/FieldCatalogCache.h` (~L14 docstring): add an explicit `if (bk == "Linear") return "Linear|" + NormalizeEndpointForCache(cfg.LinearBaseUrl) + "|" + cfg.LinearTeamId + "|" + projectKey;` arm (GitHub currently has none and aliases into the `Jira|…` shape — don't let Linear collide).

**Factory, sync, draft flow (modified)**:

14. `src/Tracker/DefaultTrackerBackendFactory.cpp` (~L17): add `if (lower == "linear") return std::make_unique<LinearClient>(cfg.LinearBaseUrl, cfg.LinearApiKey);` and fix the stale "two/three backends" header doc-comment.
15. `src/Sync/TicketSyncService.cpp` (`SwapBackendIfTrackerChanged`, ~L486–512): add a `linear` swap branch with stale-grid-clear parity; the streaming worker already calls `FetchIssuesStreamed` (~L605), satisfied by the override in #3.
16. `src/Tracker/ProjectResolver.cpp` (~L81): Linear identifiers are `TEAM-123`-shaped, so the Jira-like prefix path should resolve the team key without a special case — **verify** during implementation; add a `GetTrackerType() == "Linear"` guard only if the prefix logic misbehaves (mirroring the Plane prefix-less special-case at ~L93). Linear `project` must **not** be treated as the draft scope.
17. `src/Ui/SmatchetNewIssueDraftUi.cpp` + `src/Ui/SmatchetProjectPicker_detail.cpp`: render the draft-scope picker as **`Team`** when the active backend is Linear (the `backendKind`/`MakeRowLabel` forks at ~L582 / detail L17 currently only split Plane vs Jira — Linear, like GitHub, would otherwise masquerade as "Jira"); extend the inherit-field ternary (~L203) to a real per-backend branch using `NewIssueInheritFieldIdsLinear`.
18. `include/NewIssueInheritDefaults.h` (top-level `include/`, not under `Tracker/`): the shared default list is Jira-flavored (`issuetype`/`components`); add a Linear-specific default (e.g. `description/priority/assignee/labels/state/project`) only if the shared list is wrong for Linear.
19. `src/AppController.cpp` (~L1878–1922, beside the GitHub/Plane fixture installers — **anchors volatile: `appcontroller-god-object-decomposition` (#1447) is refactoring this file in parallel and may relocate the install/`Initialize` wiring into an extracted service; re-verify against HEAD**): install a `LinearFixtureBackend` factory when `SMATCHET_TEST_LINEAR_BACKEND_FIXTURE` is set and the active tracker is Linear (drives Bucket-E).

> Note: `IssueCreatePipeline` is **backend-agnostic** (takes an `ITrackerIssueMutations&` + a `cacheBackendKey` string, no tracker-type branch) — Linear plugs in via its mutations role, so **no `IssueCreatePipeline.{h,cpp}` edits** are expected beyond possibly neutralizing a stale "Jira payload" comment.

**Tests + fixtures**:

20. `tests/CMakeLists.txt`: register the new pure-helper tests + any non-network client tests (each test line followed by the production pure TUs it links — no cpr/SQLite/threads in Bucket A).
21. `tests/Core/LinearClientHelpers.test.cpp`: GraphQL error extraction, API-URL normalization, identifier parse, ISO-8601 parse, rate-limit header parse.
22. `tests/Core/LinearIssueMappingPure.test.cpp`: JSON fixture → `CachedTicket`, including nulls and missing optional relations (assignee/project/cycle).
23. `tests/Core/LinearQueryFromJql.test.cpp`: supported/unsupported translation + warning behavior (incl. `assignee=currentUser()` → `isMe`).
24. `tests/Core/ConfigManager.test.cpp` (extend): Linear config round-trip, secret-migration shape, env overrides.
25. `tests/Core/ConfigManagerViews.test.cpp` (extend): `KnownBackendKeys`↔`NormalizeViewsBackendKey` agreement for `Linear`; default Linear view shape.
26. `tests/Core/TrackerBackendFactoryConfig.test.cpp` (extend): factory builds a `LinearClient` for `TrackerType=="Linear"`; `FakeTrackerClient`/`RecordingTrackerBackendFactory` switching parity.
27. `tests/support/FakeLinearFixture.h`: scripted Linear GraphQL replies, mirroring `FakeGitHubFixture.h`/`FakePlaneFixture.h`.
28. `tests/ui/linear_deterministic_backend.test.cpp` (new, modeled on `tests/ui/jira_deterministic_backend.test.cpp`) **and** a Preferences-tracker-switch UI test (modeled on `tests/ui/funcsize_preferences_tabs.test.cpp` — note **no** preferences-tracker-combo UI test exists today); register in `tests/ui/CMakeLists.txt`.
29. `tests/fixtures/linear/*.json`: deterministic GraphQL bodies for teams, issues (search page), field catalog, and mutation success/error responses.

## Existing utilities reused

- **`ITrackerBackend` + role interfaces** (`include/ITrackerBackend.h`, `include/ITrackerIssueReader.h`, `…Connectivity.h`, `…FieldCatalog.h`, `…IssueMutations.h`, `…Collaboration.h`, `…Activity.h`): Linear implements the same mandatory virtuals as GitHub. Mandatory = `FetchIssues`/`FetchIssuesForKeys`/`ResolveDisplayValue` (Reader), `GetTrackerType`/`ProbeReachability` (Connectivity), `UpdateIssueFields`/`UpdateField`/`BuildFieldPayload` (Mutations). FieldCatalog/Collaboration/Activity are all-default (override as needed).
- **`GitHubClient` + its split TUs** (`Tracker/GitHubClient.*`, `GitHubClientHelpers.*`, `GitHubIssueSearch*.*`, `GitHubIssueSearchMapping.*`, `GitHubQueryFromJql.*`, `GitHubCommentMappingPure.*`, `GitHubFixtureBackend.*`): the structural template — copy the thin-adapter/pure-TU split verbatim.
- **`DefaultTrackerBackendFactory::Create`** (`Tracker/DefaultTrackerBackendFactory.cpp`): single construction point for backend selection.
- **`TicketSyncService`** (`Sync/TicketSyncService.cpp`): async streaming worker (`RunStreamingWorkerBody` → `FetchIssuesStreamed`) + `SwapBackendIfTrackerChanged` stale-grid clear; all Linear network I/O stays off the UI thread.
- **`TrackerPostLogged`** (`Tracker/TrackerHttpUtils.cpp` ~L220): GraphQL is POST-only — reuse logging/timeouts/retry/redirect-disabled SSL + `NetworkUsageTracker` accounting instead of raw `cpr::Post`. (`BuildTrackerHeaders`, `NormalizeBaseUrl`, `UrlEncode` also live here.)
- **`IsTrackerTransportErrorText`** (moved to the cpr-free `Tracker/TrackerHttpPure.cpp` ~L131): reuse sync connectivity classification; add Linear-specific auth/config error text to its `kHard[]` list so 200-with-`errors` auth failures aren't misread as "offline".
- **`CachedTicket`** (`include/CachedTicketTypes.h:13`): the mapping target (`id` + `fieldValues`/`fieldRichValues` maps).
- **`TrackerFieldOption`** (`Tracker/TrackerFieldSchema.h:26`): UUIDs in `::Id`, display in `::Value`.
- **`ConfigManager` secret persistence** (`Config/ConfigManager.cpp` `LoadSecretFields`/`ProtectSecretForConfig`): same DPAPI + plaintext-legacy-fallback pattern as the GitHub PAT / Plane key.
- **`ConfigManager::NormalizeViewsBackendKey` / `KnownBackendKeys` / `BackendCredentialsPresent`** (`Config/ConfigManager_Views.cpp`): per-backend view buckets, registry, and credential-presence checks.
- **`FieldCatalogCache`** (`Tracker/FieldCatalogCache.cpp`): cache Linear field-catalog snapshots by backend/endpoint/team.
- **`IssueCreatePipeline`** (`Tracker/IssueCreatePipeline.*`): backend-agnostic create/update validation + offline-queue integration; Linear plugs in via its mutations role.
- **`FakeTrackerClient`** (`tests/support/FakeTrackerClient.h`): scripted `ITrackerBackend` for sync-switch + create/update integration without live credentials.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: Linear issue mapping + cache writes happen in the existing background sync/apply budget; no per-frame GraphQL work.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: all Linear GraphQL calls run through `TicketSyncService` workers or the existing create/update/offline flows; mark any sync/update-reachable GraphQL boundary with `/* PILLAR2_WORKER_ONLY */ // est-latency: 15000ms`. Preferences save probes only through the existing visible save/probe path.
- **Pillar 3 (never crash)**: GraphQL null-heavy responses are mapped defensively in pure helpers; 200-with-`errors` is treated as failure or partial-warning, never a valid full sync. Run the sanitizer build after the mutation slice (payloads touch user-entered data + offline replay).
- **Pillar 4 (accessibility)**: Preferences uses existing ImGui controls/layout; add Bucket-E coverage for the tracker combo so the new fields stay keyboard-reachable.

## Perf-review-system gates (mandatory — diff touches `Source/Core/`)

1. **PR-fast CI** — fires. No Linear-specific perf scenario today; run `idle` (config/factory/worker reachability) and `priority-grid-scroll` once mapped Linear tickets enter the grid. If a `tracker_sync` scenario lands first, use it.
2. **Pillar 2 static scanner** — fires. Any new GraphQL helper reachable from sync/update gets reviewed for UI-thread reachability; worker-only calls carry the `PILLAR2_WORKER_ONLY` marker near the boundary.
3. **Dispatcher drain** — N/A. The plan doesn't touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — fires if Preferences save adds a live Linear probe or any sync stall. Reuse the existing sync toast/progress affordances; add/extend a UI test if a new stall path appears.
5. **Marker inventory** — N/A. No new `SMATCHET_UI_PERF_SCOPE` markers planned.

**Pre-push local check**: `bash scripts/dev/perf-run.sh idle priority-grid-scroll` (+ any Linear scenario added during implementation).
**Override**: `perf-out-of-band` PR label only for an intentional regression with a queued baseline/follow-up.

## Risks / non-goals

- **GraphQL 200-with-errors**: easy to misclassify as success. Mitigation: helper checks `errors` before reading `data`; partial data → warning only when the page is usable; add Linear auth/config strings to `IsTrackerTransportErrorText`'s `kHard[]`.
- **Auth header shape**: Linear personal keys use `Authorization: <key>` with **no** `Bearer` (unlike GitHub). Mitigation: `LinearClientHelpers` builds the header explicitly + a unit test pins the exact header bytes.
- **Backend-name casing trap**: env-override compares raw `cfg.TrackerType`, the registry uses PascalCase, the factory/sync lowercase. Mitigation: `GetTrackerType()` returns `"Linear"`, env arms compare case-insensitively, and the `KnownBackendKeys`↔`NormalizeViewsBackendKey` CI guard is satisfied in the same commit.
- **Linear filter semantics ≠ JQL**: full parity is unrealistic. Mitigation: limited translator with warnings + tests, no silent broadening beyond safe clauses.
- **Team vs project terminology**: Smatchet's draft scope is internally `ProjectKey`. Mitigation: keep that as plumbing; Linear UI/docs say `Team`; Linear `project` stays a mutable issue field.
- **Option display names aren't stable IDs**: states/labels/users/projects/cycles need UUIDs. Mitigation: field catalog stores UUIDs in `TrackerFieldOption::Id`, display names only for UI.
- **Pagination + rate + complexity limits**: Linear is cursor-paginated and dually rate-limited (requests + complexity). Mitigation: cap pages, soft-warn at cap, keep catalog queries small, and log the `x-ratelimit-*`/`x-complexity` headers in diagnostics.
- **Large field catalogs on big teams**: members/labels/projects may be numerous. Mitigation: cap/search where Linear supports it; keep the initial catalog to fields create/update need.

## Non-goals

- **Linear OAuth app flow** (shared/team installs) — personal API key only for v1; OAuth is a future plan.
- **Webhook-driven incremental sync** — future; polling parity with Jira/Plane is enough.
- **Attachment upload/download** — future (Linear file-storage auth is its own flow).
- **`ITrackerActivity` integration** (user-activity feed) — the role is optional/default; skip for the Linear MVP.
- **Linear customer objects, initiatives, documents, relations, estimates UX, roadmap** — out of the tracker surface.
- **Runtime GraphQL introspection** — hand-written queries are smaller and adequate.
- **Linear-native query UI** — follow-up only if the JQL-subset translator proves too limiting.
- **Multi-team aggregation** — single active team first; multi-team needs its own UX + more pagination/rate work.

## Verification

- **Bucket A (pure-logic ctest, `SmatchetTests` doctest rig)**: `LinearClientHelpers`, `LinearIssueMappingPure`, `LinearQueryFromJql`, config round-trip/env override, `KnownBackendKeys`/`NormalizeViewsBackendKey` agreement, factory builds `LinearClient`, GraphQL error-parser tests.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: Preferences tracker-switch scenario (Linear appears, config buffers render, save persists the profile, backend-kind switch clears old active tickets) + a `LinearFixtureBackend` deterministic replay via `SMATCHET_TEST_LINEAR_BACKEND_FIXTURE`.
- **Sanitizer**: run the sanitizer build after the mutation slice (payload building touches user-entered JSON-like data + offline replay). No screenshot/golden expected.
- **Build gate (dual-target)**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`.
- **Manual residue**: one live Linear smoke remains unavoidable unless CI gains test credentials — configure a personal API key, fetch one team, sync issues, update a throwaway issue title/label, add a comment, create a test issue. If still manual at ship time, track an automation follow-up in `docs/self-improvement/categories/tooling/`.

## Implementation log

Shipped on branch `claude/linear-tracker-slice1` (PR #1453). One bullet per commit, newest last:

- `d21399e` · slice 1 — pure GraphQL/parse helpers (`LinearClientHelpers`): issue-key parse/format, API-URL normalize, GraphQL body build, raw auth-header (no Bearer), error + rate-limit header parse, request-auth resolution (#979).
- `180d744` · slice 1 — pure `LinearIssueMappingPure`: null-safe `issues.nodes` → `CachedTicket` (summary/description/status/assignee/labels/author/created/updated/priority/project/team/cycle/url), priority-label fallback to the 0–4 map.
- `656ceff` · slice 1 — `LinearQueryFromJql`: conservative JQL-subset → Linear `IssueFilter` translator (assignee/status/labels/priority/project/team/text), drops unsupported clauses with a Warning.
- `46c2140` · slice 1 — `LinearClient` read-only shell contract (multiply-inherits the role interfaces; `GetTrackerType()=="Linear"`; per-request auth re-resolution).
- `9b139d1` · slice 1 — clear comment-noise + decompose `ProcessClause` into `DetectOperator`/`Handle*` helpers under the 120-line / 30-branch caps.
- `584dde8`…`246f2d3` · slice 2 — integration layer (config fields + DPAPI-encrypted key + registration-table persistence + env routing, factory branch, `TicketSyncService` swap branch, Preferences UI panel, FieldCatalog cache namespace, new-issue/picker/grid arms), decomposed and validated.
- `5d5d78d` · slice 2 — MSVC build fix: drop the function-call NSDMI on `JqlToLinearResult::Filter` (cross-TU name-lookup); default-null + lazy `operator[]`.
- `dec2ee8` · slice 2 — extend the ConfigManager Save/Load round-trip test to cover the Linear config fields.
- `c6ea0ab` · slice 3 — writes: `issueUpdate` / `issueCreate` / `commentCreate` (`LinearIssueMutation.cpp`), with identifier→UUID resolution before each mutation; `Collaboration()` self-returns; Preferences UI + draft-inherit wiring.
- `20f4069` · slice 4 — read-only `LinearFixtureBackend` + `SMATCHET_TEST_LINEAR_BACKEND_FIXTURE` AppController hook + `LinearDeterministic` bucket-E test + fixture + runner.
- `ea3b46a` · bucket-A integration — factory swap-path carries live Linear creds; `LoadViewsOrBootstrap` Linear default field-set; `NormalizeViewsBackendKey` Linear cases.

## Deviations from plan

- **`JqlToLinearResult.Filter` is default-null, not `= nlohmann::json::object()`** — the function-call default-member-initializer tripped MSVC's cross-TU name lookup (clang accepted it). Null + lazy `operator[]` is behaviourally equivalent; `HasFilter()` treats null/empty as "no filter".
- **`ProcessClause` decomposed** into `DetectOperator` + `HandleAssignee/Status/Labels/Priority` to satisfy the 120-line / 30-branch caps (the monolithic version measured 130L / 43br once the base ratchet refreshed).
- **`Activity()` stays `nullptr`** — the activity-feed role is out of MVP scope (the plan flagged it nullable). `Collaboration()` self-returns for `commentCreate`.
- **`LinearFixtureBackend` uses a free-function factory** (`MakeLinearFixtureBackendFactory`, mirroring `PlaneFixtureBackend`) rather than GitHub's inline-class factory — keeps `AppController.cpp` thin, which matters while #1447 decomposes that TU.
- **Bucket-E Preferences tracker-switch scenario folded into Bucket-A** — "save persists the profile" is pinned by the ConfigManager round-trip (`dec2ee8`); "backend-kind switch clears old active tickets" by the factory swap-path test (`ea3b46a`) + the existing `SwapBackendIfTrackerChanged` suite. The dedicated bucket-E coverage is the `LinearDeterministic` fixture replay; a separate Linear Preferences ImGui test would largely duplicate that Bucket-A coverage with a more brittle UI-driven assertion.

## Verification (actual)

- **POSIX core compile gate** (`cmake --preset posix-core-check && cmake --build … --target SmatchetCore_PosixCheck`): **passed** locally on host clang 18 — all six cpr-bound Linear TUs (`LinearClient`, `LinearIssueSearch`, `LinearIssueMutation`, plus the three pure TUs), `LinearFixtureBackend.cpp`, and the edited `AppController.cpp` compile clean.
- **Bucket A** (`SmatchetTests` doctest rig): `LinearClientHelpers`, `LinearIssueMappingPure`, `LinearQueryFromJql`, ConfigManager Linear round-trip, factory swap-path, and `LoadViewsOrBootstrap`/`NormalizeViewsBackendKey` Linear cases written + registered; `clang++ -std=c++14 -fsyntax-only` clean on the changed TUs. **Full ctest run: via CI** (links cpr/SQLite, not reproducible in this container).
- **Bucket E** (`ninja-ui-test-msvc`): `LinearDeterministic` group (`Sync_LoadsIssuesIntoGrid`, `Sync_MapsLinearSpecificFields`) written + registered; runs in the MSVC UI-test CI. Driver: `scripts/dev/test-ui-linear-deterministic-backend.sh`.
- **Lint / high-integrity gate**: **passed** (`test-lint-rules.sh --diff origin/develop`) — no new strict-zone, comment-noise, or oversized-function violations. Duplication clones vs the sibling GitHub/Jira/Plane backends are WARN-only (calibration phase) and carry `SMATCHET_DEVIATION(rule=duplication)` on the interface-mandated override symmetry.
- **Live Linear smoke**: **NOT RUN** — no CI test credentials. Remains the manual residue called out above (configure a personal API key, fetch a team, sync, update an issue, add a comment, create a test issue). Automation tracked as a tooling follow-up if still manual at ship.
