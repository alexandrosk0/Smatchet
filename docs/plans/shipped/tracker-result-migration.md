# Plan — Tracker backend `Result<T, TrackerError>` migration (hardening #21b)

> **Slug**: `tracker-result-migration`
>
> **Status**: `shipped` — all cited PRs merged (see Implementation log); archived 2026-06-16 via plan-archival sweep.
>
> **Usage**: execution-ready slicing plan. Each slice below is an independently-compiling, independently-shippable PR. Implement one slice per PR.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section. Per `AGENTS.md` § Plan revision after implementation, the agent that ships each slice appends to § Implementation log / § Deviations / § Verification (actual) in the same post-ship PR.

## Context

Hardening item #21b: retire the `bool foo(..., std::string& outError)` error-passing convention across the tracker backend layer in favour of the project-local `Result<T, TrackerError>` / `Optional<T>` (`Source/Core/include/SmatchetResult.h`, #1017) + `TrackerError` (`Source/Core/include/Tracker/TrackerError.h`) value types. The user chose the **FULL** `ITrackerBackend` virtual-interface migration, not a bounded slice.

Measured scope: 285 first-party `std::string& outError` sites; 52 are virtual (`= 0` / `override`) across the five `ITracker*` sub-interfaces and their backends (Jira / GitHub / Plane + production fixtures + test fixtures). After this lands, the tracker backend interfaces and their immediate callers carry typed, self-describing errors instead of an out-param string + bool. Pure refactor — zero behaviour change.

## Approach

`ITrackerBackend` is a **facade over five sub-interfaces** (`ITrackerIssueReader`, `ITrackerConnectivity`, `ITrackerFieldCatalog`, `ITrackerIssueMutations`, `ITrackerCollaboration`). `ITrackerConnectivity` has **zero `outError` methods** — entirely out of scope. The migration is sliced **by method family across one sub-interface at a time**: interface decl + every override (concrete backends + production fixtures + test fixtures) + every direct caller change together (a virtual is all-or-nothing per method or it won't compile).

**Return-shape rule (the core decision):**
- **Payload-bearing** methods (`bool foo(..., OutPayload&, outError)`, or `std::string foo(...)` returning id-or-empty) → **`Result<Payload, TrackerError>`** (the payload becomes the Ok value).
- **Pure success/fail** methods (no out-payload — `UpdateIssueFields`, `AddIssueWatcher`, etc.) → **bare `TrackerError` return**. `Result<void>` does NOT compile (`std::aligned_union<0, void, E>` is ill-formed), and `TrackerError` already self-describes ok-ness via `IsOk()`. A `Result<Unit, TrackerError>` would double-wrap the no-error state the error type already encodes. So: return `TrackerError`; caller checks `if (!e.IsOk())`. The two idioms (`Result` vs bare `TrackerError`) are a *feature* — the signature tells the caller whether data comes back.
- **Multi-out-param** methods → a small payload struct as the Ok value (see § Interface contracts).
- **Pure non-HTTP validators/parsers** (`GitHubClientHelpers`) → **`Result<T>` with the default `E = std::string`** (their error is a hand-written message, not a `TrackerError`).

**The chokepoint is the `AppController` public wrapper, not the UI.** UI / Command / Grid callers (`AnnotateAnalysisUi_*`, `BuiltinCommands_*`, `TrackerGridFieldDisplay`, `SmatchetAutocompleteUi`, `SmatchetUI`) call `AppController`'s own `bool ... outError` methods, **never the `ITracker*` virtuals directly**. The only direct callers of the virtuals are `AppController_CatalogAndFieldEdit.cpp`, `AppController.cpp`, `Sync/OfflineQueueService.cpp`, `Tracker/IssueCreatePipeline.cpp`, and the backends internally. Therefore each virtual-migration slice changes the virtual + its overrides + adapts the single `AppController` wrapper, where it **translates `Result`/`TrackerError` back to the wrapper's existing `bool + outError` public signature** (`if (!r) { outError = r.error().Detail; return false; }`). UI stays untouched, keeping every slice small. Flipping the `AppController` public wrappers themselves to `Result` (which *does* ripple to UI) is an explicit later phase (Slices 8–9), not part of the virtual-interface migration.

## Interface contracts — exact target signatures

New payload structs (define in the slice that first needs them; suggested home noted):

```cpp
// ITrackerFieldCatalog.h — FetchProjectComponents multi-out
struct TrackerProjectComponents {
    std::vector<TrackerComponent>   Components;
    std::vector<TrackerFieldOption> Options;
};
// ITrackerCollaboration.h — FetchIssueVotes multi-out (was 3 optional out-pointers)
struct TrackerIssueVotes {
    std::vector<TrackerUser> Voters;
    int  VoteCount = 0;
    bool HasVoted = false;
    bool VotersArrayInResponse = false;
};
```

| Current virtual | Target |
|---|---|
| `bool FetchFieldCatalog(cfg, projectKey, TrackerFieldCatalogResult& outCatalog, outError)` | `Result<TrackerFieldCatalogResult, TrackerError>` |
| `bool FetchIssueEditMeta(cfg, id, unordered_map<string,bool>& out, outError)` | `Result<std::unordered_map<std::string,bool>, TrackerError>` |
| `bool FetchProjectComponents(cfg, key, vector<TrackerComponent>&, vector<TrackerFieldOption>&, outError)` | `Result<TrackerProjectComponents, TrackerError>` |
| `bool BuildFieldPayload(field, values, json& outPayload, outError)` | `Result<nlohmann::json, TrackerError>` |
| `bool BuildCreatePayload(draft, catalog, json& outPayload, outError)` | `Result<nlohmann::json, TrackerError>` |
| `bool BuildUpdatePayload(draft, catalog, json& outPayload, outError)` | `Result<nlohmann::json, TrackerError>` |
| `std::string CreateIssue(fields, outError)` | `Result<std::string, TrackerError>` |
| `bool AttachFilesToIssue(key, paths, vector<pair<string,string>>& outFailures, outError)` | `Result<std::vector<std::pair<std::string,std::string>>, TrackerError>` (Ok payload = per-file failures list, possibly empty; Err = hard failure — see landmine L3) |
| `bool UpdateIssueFields(id, fields, outError)` | `TrackerError` (void-payload) |
| `bool UpdateField(id, field, values, outError)` | `TrackerError` (void-payload) |
| `bool AddIssueToSprint(key, sprintId, outError)` | `TrackerError` (void-payload) |
| `bool FetchIssuesForKeys(cfg, keys, views, vector<CachedTicket>& out, outError)` | `Result<std::vector<CachedTicket>, TrackerError>` |
| `bool FetchIssueWatchers(cfg, key, vector<TrackerUser>& out, outError)` | `Result<std::vector<TrackerUser>, TrackerError>` |
| `bool FetchIssueVotes(cfg, key, vector<TrackerUser>& out, outError, int*, bool*, bool*)` | `Result<TrackerIssueVotes, TrackerError>` |
| `bool SearchUsersByQuery(cfg, query, vector<TrackerUser>& out, outError)` | `Result<std::vector<TrackerUser>, TrackerError>` |
| `bool FetchUserGroupNames(cfg, accountId, vector<string>& out, outError)` | `Result<std::vector<std::string>, TrackerError>` |
| `bool FetchIssueComments(key, vector<TrackerIssueComment>& out, outError)` | `Result<std::vector<TrackerIssueComment>, TrackerError>` (see landmine L5 — no override/caller today) |
| `bool AddIssueWatcher(cfg, key, outError)` | `TrackerError` |
| `bool AddIssueCommentPlain(cfg, key, text, outError)` | `TrackerError` |
| `bool AddWorklog(cfg, key, …, outError)` | `TrackerError` |
| `bool AddIssueCommentAnnotateContext(cfg, …, outError)` | `TrackerError` |
| Non-virtual `bool IsValidGitHubBaseUrl(url, outError)` | `Result<Unit?> ` → use **`Result<std::string, std::string>`** carrying the normalized URL, OR keep `Result<bool>`; recommend `Optional<std::string>` error: implement as `Result<std::monostate?>`. **Decision: `std::string IsValidGitHubBaseUrl(...)` returning empty-on-valid is rejected; use `Result<bool, std::string>::Err(msg)` / `::Ok(true)`** (default-E). See Slice 1. |
| Non-virtual `int64 ParseIso8601ToUnixSec(iso, outError)` | `Result<std::int64_t, std::string>` (default-E) |
| Non-virtual `bool BuildGitHubCreatePayload(..., json& out, outError)` | `Result<nlohmann::json, std::string>` (default-E) |

**`TrackerError` construction at call sites — reuse the existing classifier, do NOT invent one.** `Source/Core/include/Tracker/TrackerHttpClient.h` already exposes `ClassifyTrackerResponse(const cpr::Response&) -> TrackerHttpResult{ TrackerError Error; cpr::Response Response; }` and `TrackerError.h` exposes `TrackerErrorFromHttpStatus(int status, std::string detail)` (401/403→Auth, 404→NotFound, 429→RateLimited, 5xx→ServerError, other-4xx→InvalidRequest, ≤0→Transport). Backends that already route through `TrackerHttpClient` get the `TrackerError` for free from `result.Error`. Backends still on raw `cpr::Get/Post` (e.g. some `JiraUserAndMeta` / `PlaneIssueMutation` paths) construct via `TrackerErrorFromHttpStatus(resp.status_code, body)`; pure-parse failures use `TrackerErrorParse(msg)`; validation/precondition failures use `TrackerErrorInvalidRequest(msg)`; "unsupported by this backend" interface defaults use `TrackerErrorInvalidRequest("X is not supported by this backend.")` (preserves the current message verbatim → zero behaviour change for callers that surface `.Detail`).

**`SMATCHET_EMBEDDED_IN_UNREAL` / `SMATCHET_WITH_MCP`**: no impact. `Result`/`Optional` are header-only C++14, no new includes leak into `Source/Core/include` beyond `SmatchetResult.h` + `TrackerError.h` (both already core). `Source/Plugins/Mcp/` does **not** call any tracker `outError` method (verified — no `std::string& outError` site in `Source/Plugins/Mcp/`); MCP wire-format unaffected.

## Override / caller matrix (lockstep set per family)

Production fixtures (`GitHubFixtureBackend`, `PlaneFixtureBackend`) implement **only** the Reader subset (`FetchIssues`, `FetchIssuesForKeys`, `ResolveDisplayValue`) + Mutations subset (`UpdateIssueFields`, `UpdateField`, `BuildFieldPayload`). They do **not** override `ITrackerFieldCatalog` or `ITrackerCollaboration` (use interface defaults). Test fixture `tests/support/FakeTrackerClient.h` overrides the full Mutations + Reader-keys set; `JiraFakeTrackerFixture` overrides `FetchFieldCatalog`.

- **FieldCatalog** overrides: Jira (`TrackerFieldCatalog.cpp`, `JiraUserAndMeta.cpp:189` editmeta), GitHub (`GitHubClient.cpp:265,316`), Plane (`PlaneFieldCatalog.cpp:367,466`). FetchProjectComponents is **Jira-only**. No production/test fixture except `JiraFakeTrackerFixture` + `JiraCatalogHttpFixture`.
- **Mutations builders/writes** overrides: Jira (`JiraIssueMutation.cpp`), GitHub (`GitHubClient.cpp` — stubs), Plane (`PlaneIssueMutation.cpp`), prod fixtures (`GitHubFixtureBackend.cpp`, `PlaneFixtureBackend.cpp` — subset), `FakeTrackerClient.h`.
- **FetchIssuesForKeys** overrides: Jira (`JiraIssueSearch.cpp:370`), GitHub (`GitHubClient.cpp:251` → `GitHubIssueSearch.cpp:530 FetchIssuesForKeysViaRestApi`), Plane (`PlaneIssueSearch.cpp:542`), both prod fixtures, `FakeTrackerClient.h`.
- **Collaboration** overrides: **Jira-only** (`JiraUserAndMeta.cpp`, `JiraIssueMutation.cpp`) — GitHub/Plane/all fixtures use interface defaults.

Direct virtual callers (the only files that change at the call boundary): `AppController_CatalogAndFieldEdit.cpp`, `AppController.cpp:957`, `Sync/OfflineQueueService.cpp:843,1082`, `Tracker/IssueCreatePipeline.cpp:75,106,168,264,272,334,342`.

## Slices (ordered — implement one PR each)

> Note (Slice 1 review): `IsValidGitHubBaseUrl` lands as `Result<bool,std::string>` whose Ok payload is always `true` (validity is encoded by Ok-vs-Err). Faithful 1:1 of the old `bool` return; a later cleanup could collapse it to a value-less success shape — do NOT read the `bool` as meaningful payload.

**Slice 1 — GitHubClientHelpers pure validators/parsers** (no virtual dispatch; validates `Result` + strict-zone lint mechanics first). Methods: `IsValidGitHubBaseUrl` → `Result<bool,std::string>`, `ParseIso8601ToUnixSec` → `Result<std::int64_t,std::string>`, `BuildGitHubCreatePayload` → `Result<nlohmann::json,std::string>`. Files: `Tracker/GitHubClientHelpers.h`, `Tracker/GitHubClientHelpers.cpp`, callers `Tracker/GitHubClient.cpp`, `Tracker/GitHubIssueSearch.cpp`, `Diagnostics/BugReportBody.cpp`, + any `tests/Core/GitHubClientHelpers*.test.cpp`. **~6 files.**

**Slice 2 — FieldCatalog family** (first virtual; no fixture lockstep). Methods: `FetchFieldCatalog`, `FetchIssueEditMeta`, `FetchProjectComponents`. Files: `ITrackerFieldCatalog.h` (+`TrackerProjectComponents` struct), `Tracker/JiraClient.h`, `Tracker/GitHubClient.h`, `Tracker/PlaneClient.h`, `Tracker/TrackerFieldCatalog.cpp`, `Tracker/JiraUserAndMeta.cpp`, `Tracker/GitHubClient.cpp`, `Tracker/PlaneFieldCatalog.cpp`, callers `AppController_CatalogAndFieldEdit.cpp` (wrappers @120,145,160,596,679,794 translate to existing `AppController::FetchFieldCatalog` bool signatures), `tests/support/JiraFakeTrackerFixture.cpp`, `tests/support/JiraCatalogHttpFixture.h`, `tests/Core/TrackerCatalogBuild.test.cpp`, `tests/Core/JiraEditMetaPure.test.cpp`. **~13 files.** Note: Jira's non-virtual 5-vector `FetchFieldCatalog` overload (`JiraClient.h:66`) stays `bool + outError` internal (flag L1); the virtual delegating to it unwraps.

**Slice 3 — Mutations payload builders.** Methods: `BuildFieldPayload`, `BuildCreatePayload`, `BuildUpdatePayload` → `Result<nlohmann::json, TrackerError>`. Files: `ITrackerIssueMutations.h`, 3 backend `.h`, `Tracker/JiraIssueMutation.cpp`, `Tracker/GitHubClient.cpp`, `Tracker/PlaneIssueMutation.cpp`, `Tracker/GitHubFixtureBackend.cpp` (BuildFieldPayload), `Tracker/PlaneFixtureBackend.cpp` (BuildFieldPayload), `tests/support/FakeTrackerClient.h`, callers `AppController_CatalogAndFieldEdit.cpp:459,1069`, `Tracker/IssueCreatePipeline.cpp:264,334`. **~13 files.** Internal `UpdateField` (Jira/Plane) calls `BuildFieldPayload` then unwraps — stays decoupled.

**Slice 4 — Mutations void-payload writes.** Methods: `UpdateIssueFields`, `UpdateField`, `AddIssueToSprint` → bare `TrackerError`. Files: `ITrackerIssueMutations.h`, 3 backend `.h` (+ Jira non-virtual `AddIssueToSprint(cfg,…)` overload + private `UpdateIssueFieldsViaTransition/ViaPut` — flag L2), `Tracker/JiraIssueMutation.cpp`, `Tracker/GitHubClient.cpp`, `Tracker/PlaneIssueMutation.cpp`, both prod fixtures (`UpdateIssueFields`,`UpdateField`), `tests/support/FakeTrackerClient.h`, callers `AppController_CatalogAndFieldEdit.cpp:932,1012,1080,1097,1295,1307,1335,1384`, `Sync/OfflineQueueService.cpp:1082`, `Tracker/IssueCreatePipeline.cpp:75,106,272`. **~15 files.**

**Slice 5 — Mutations create/attach payload-returning.** Methods: `CreateIssue` → `Result<std::string,TrackerError>`, `AttachFilesToIssue` → `Result<vector<pair<string,string>>,TrackerError>`. Files: `ITrackerIssueMutations.h`, 3 backend `.h`, `Tracker/JiraIssueMutation.cpp`, `Tracker/GitHubClient.cpp`, `Tracker/PlaneIssueMutation.cpp`, `tests/support/FakeTrackerClient.h`, callers `Tracker/IssueCreatePipeline.cpp:168,342`. (`CreateIssue` reaches UI only via `CreateIssueAsync`/`IssueCreatePipeline`, which keeps its `IssueCreateResult` struct → no UI change.) **~9 files.**

**Slice 6 — Reader `FetchIssuesForKeys`** → `Result<vector<CachedTicket>,TrackerError>`. Files: `ITrackerIssueReader.h`, 3 backend `.h`, `Tracker/JiraIssueSearch.cpp`, `Tracker/GitHubClient.cpp`, `Tracker/GitHubIssueSearch.{h,cpp}` (`FetchIssuesForKeysViaRestApi`), `Tracker/PlaneIssueSearch.cpp`, both prod fixtures, `tests/support/FakeTrackerClient.h` (+`SetFetchIssuesForKeysResult`), `tests/support/FakeGitHubFixture.h`, callers `AppController.cpp:957`, `Sync/OfflineQueueService.cpp:843`. **~14 files.**

**Slice 7 — Collaboration (Jira-only overrides).** Split if over ceiling:
- **7a (reads)**: `FetchIssueWatchers`, `FetchIssueVotes` (+`TrackerIssueVotes` struct), `SearchUsersByQuery`, `FetchUserGroupNames`, `FetchIssueComments`. Files: `ITrackerCollaboration.h`, `Tracker/JiraClient.h`, `Tracker/JiraUserAndMeta.cpp`, `AppController_CatalogAndFieldEdit.cpp` wrappers @1462,1515,1542,1665 (translate to existing `AppController` bool signatures), `AppController.h`. **~5 files.**
- **7b (writes)**: `AddIssueWatcher`, `AddIssueCommentPlain`, `AddWorklog`, `AddIssueCommentAnnotateContext` → bare `TrackerError`. Files: `ITrackerCollaboration.h`, `Tracker/JiraClient.h`, `Tracker/JiraUserAndMeta.cpp`, `Tracker/JiraIssueMutation.cpp`, `AppController_CatalogAndFieldEdit.cpp` wrappers @1487,1568,1619,1630. **~5 files.**

**Slices 8–9 (FOLLOW-ON phase, beyond the virtual interface).** Flip the `AppController` public wrappers from `bool + outError` to `Result`/`TrackerError`, rippling to UI/Command callers. **Status: SCOPED + APPROVED-AS-SHAPE, not yet executed** (user reviewed the breakdown 2026-06-08 and chose "commit the scoping, stop" — Slices 1–7 stand as the delivered #21b goal; 8–9 is ready-to-execute follow-on, no code written).

> **ROI note (carry into execution):** lower-value than 1–7. At the UI edge nearly every caller does `if (!ok) showToast(err)`, which `bool + outError` already serves; the win from 8–9 is *idiom consistency* + deleting the wrapper translation layer, NOT new safety. Reasonable to do 8c+8a and defer 8b. **Visual-validation:** every 8x PR touches `Smatchet*Ui*.cpp` / `AnnotateAnalysisUi*.cpp` → trips the visual-validation exception; it is a pure refactor (no visual change) so build + bucket-A cover it, but the orchestrator pauses with a launched exe per UI-touching PR unless the user pre-authorises "refactor-only, skip visual check".

**Already insulated — NOT part of 8–9** (verified): `CreateIssueAsync` returns `std::future<IssueCreateResult>` (struct, no `outError`) — done by Slice 5; `IssueCreatePipeline::Run` returns `IssueCreateResult` and its steps already call the migrated `Result` virtuals.

Per-PR seam split (each its own PR, off fresh develop after the prior merges, worktree reused; recommended order **8c → 8a → 8b**, smallest/lowest-risk first):

- **Slice 8c — Field-catalog public wrappers** (S). Flip `FetchFieldCatalog` (2 overloads) + `RefreshFieldCatalog` → `Result<TrackerFieldCatalogResult, TrackerError>`. Callers: `SmatchetUI.cpp:243` (`FetchFieldCatalog`) + internal. **~3 files.**
- **Slice 8a — Collaboration public wrappers** (L). Flip the 8 reads/writes — `FetchIssueWatchers`, `AddIssueWatcher`, `FetchIssueVotes` (returns `TrackerIssueVotes` directly, dropping the out-pointer wrapper), `SearchUsersByQuery`, `AddIssueCommentPlain`, `SubmitWorklog`, `AddIssueCommentAnnotateContext`, `FetchUserGroupNames`. Caller files (~15 sites): `Commands/Builtin/BuiltinCommands_Users.cpp` (@30,54,79), `Commands/Builtin/BuiltinCommands_TicketMutations.cpp` (@81,118), `Ui/AnnotateAnalysisUi_Modals.cpp` (@30,310,325,371), `Ui/AnnotateAnalysisUi_Window.cpp` (@805,851,900), `Ui/SmatchetAutocompleteUi.cpp` (@406), `Ui/SmatchetGridUiSupport.cpp` (@202), `Ui/TrackerGridFieldDisplay.cpp` (@702,734,788) + `AppController.h` + `AppController_CatalogAndFieldEdit.cpp` (the wrappers stop translating — return the `Result`/`TrackerError` straight through). **~10 files.**
- **Slice 8b — Field-edit public wrappers** (M, hottest UI path — grid cell edits). Flip public `SubmitFieldEdit` + `SubmitFieldEditNetworkOnly` → `Result`/`TrackerError` (keep the private `Submit*FieldEdit*`/`ApplyFieldUpdateWithEditMetaRetry` helpers + `FieldEditResult` struct as-is — already result-shaped). Callers: `Commands/Builtin/BuiltinCommands_TicketMutations.cpp` (@56,155,194), `Ui/AnnotateAnalysisUi_Window.cpp` (@774,897), the Lua `AppController::Impl::SubmitFieldEdit` (`AppController_LuaBindings.cpp:741` + `AppController_LuaStubs.cpp`), + the grid field-edit pipeline (`SmatchetGridFieldEditPipeline` — verify call path at execution). **~6 files.** Defer-candidate (lowest ROI / highest churn).
- **Slice 9 — non-virtual helpers** (S, no UI ripple; foldable into 8b/8c). `IssueDraftHelpers::FromJson`, `FieldCatalogCache` free fns, `TrackerFieldPayload.cpp` facade — pure-logic `bool + outError` → `Result<T, std::string>` (default-E) per Slice-1 precedent.

## Files to modify

See per-slice file lists above (authoritative). Grep-verified call inventory captured inline so the implementer renames exhaustively. Cross-cutting headers touched in multiple slices: `ITrackerIssueMutations.h` (Slices 3,4,5), `ITrackerCollaboration.h` (7a,7b) — each slice touches a disjoint method subset within the header, so no merge conflict if shipped in order.

## Existing utilities reused

- `ClassifyTrackerResponse` / `TrackerHttpResult` — `Source/Core/include/Tracker/TrackerHttpClient.h:40` — backends on the HTTP-client path get `TrackerError` from `result.Error` directly.
- `TrackerErrorFromHttpStatus(status, detail)` — `Source/Core/include/Tracker/TrackerError.h:97` — canonical HTTP-status→kind classifier (do NOT add a parallel one).
- `TrackerErrorParse` / `TrackerErrorInvalidRequest` / `TrackerErrorTransport` factories — `TrackerError.h:66-92`.
- `Result<T,E>::Ok/::Err`, `Optional<T>` — `Source/Core/include/SmatchetResult.h:108,116`.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no impact. `Result`/`Optional` are in-object aligned-storage value types (no heap alloc beyond the moved-in payload, which is the same payload the out-param carried). Pure refactor, perf-neutral by construction.
- **Pillar 2 (UI never blocks > 100 ms)**: no impact — no call-site is moved on/off the worker thread; the existing worker-dispatch wrappers in `AppController` / `AnnotateAnalysisUi` are preserved.
- **Pillar 3 (never crash)**: net positive — `Result::value()`/`error()` on the wrong state assert+throw (Pillar-3 safe) vs the silent-garbage risk of reading an unset out-param after a `false` return. No new `catch(...)`.
- **Pillar 4 (accessibility)**: N/A — no UI surface change.

## Perf-review-system gates (diff touches `Source/Core/` → applies)

1. **PR-fast CI** — scenario: `tracker-sync` / `field-edit` exercise the changed mutation + reader paths. Run the curated subset per `agents/core/perf-gatekeeper.md`. **Perf-neutral by construction** (value-type swap, no new work, no allocation-pattern change) — expect zero delta; a regression signals an accidental copy (e.g. `Result` returned by value but payload copied instead of moved — verify `std::move` on `r.value()` extraction at call sites).
2. **Pillar 2 static scanner** — N/A: no new sync-I/O reachable from `ImGui::*` (no call relocated).
3. **Dispatcher drain** — N/A: does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — N/A: no new > 100 ms stall path.
5. **Marker inventory** — N/A: adds no `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check Step 7 against `tracker-sync` once on the largest slice (Slice 4) to confirm the value-type swap is neutral.

**Override**: `perf-out-of-band` not expected to be needed.

## Risks / non-goals

- **L1 (Jira 5-vector `FetchFieldCatalog` overload)**: Jira keeps a non-virtual `bool FetchFieldCatalog(cfg, key, vector<Field>&, vector<Component>&, vector<IssueTypeMeta>&, outError)` (`JiraClient.h:66`) that the Result-returning virtual assembles from. **Mitigation**: keep this internal helper `bool + outError`; the virtual unwraps it. Migrating it would need a 3-vector struct for marginal gain — `SMATCHET_DEVIATION(rule=…; reason=internal Jira assembly helper; revisit=phase-8)` or simply leave (non-virtual, out of the interface scope).
- **L2 (Jira private `UpdateIssueFieldsViaTransition/ViaPut` + `AddIssueToSprint(cfg,…)`)**: private/non-virtual helpers, `bool + outError`. Keep internal; public override unwraps. Same deviation note.
- **L3 (`AttachFilesToIssue` dual-channel semantics)**: current contract returns `true` when every file uploaded, with per-file failures collected in `outFailures` **even on the true path** (partial success). **Mitigation**: map Ok payload = the (possibly non-empty) failures vector; Err = hard failure only. Caller `IssueCreatePipeline.cpp:168` reads `result.AttachmentFailures` from the Ok payload. Do NOT collapse "some files failed" into an Err — that would change behaviour.
- **L4 (`FetchIssues` deliberately OUT OF SCOPE)**: `FetchIssues(bool* outFullSyncCompleted, …, std::string* outFetchError, std::string* outWarning)` uses **pointer-out params with defaults** (not the `std::string& outError` convention) and returns a `vector<CachedTicket>`; it already has the `TrackerIssueFetchSummary` (FetchError + Warning) result-struct path via `FetchIssuesStreamed`. Migrating it ripples into the sync engine + every fixture's default-arg signature. **Excluded by design** — separate effort if ever wanted.
- **L5 (`FetchIssueComments` has no override + no caller)**: declared on `ITrackerCollaboration` with a default body; no backend overrides it, no caller invokes it (Jira uses internal `JiraFetchIssueCommentsPages`). **Mitigation**: migrate the interface decl for consistency in Slice 7a (cheap, no override/caller to chase) OR leave + flag dead. Recommend migrate-decl-only.
- **ABI / save-format**: none. No serialized struct changes; `TrackerFieldCatalogResult`, `CachedTicket`, config, SQLite cache schemas untouched.
- **Backend leakage into core**: none — no GLFW/GL/cpr added to `Source/Core/include`. `nlohmann/json` already included by `ITrackerIssueMutations.h`.
- **Dual-target**: `Source/Core/src/Tracker/` + `AppController*` compile in both `SmatchetStandalone` and `SmatchetCore_DX12`; build both per slice.
- **Non-goal**: not migrating non-tracker `outError` sites (Whisper, `P4Annotate`, `SubprocessCapture`, `ConfigManager`, `SmatchetImageTextureCache`) — out of the tracker layer.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps.

- **Bucket A (pure-logic ctest, `test-rig`)**: existing `tests/Core/IssueCreatePipeline*.test.cpp`, `tests/Core/OfflineQueue*.test.cpp`, `tests/Core/TrackerCatalogBuild.test.cpp`, `tests/Core/JiraFakeTrackerFixture.test.cpp`, `tests/Core/JiraEditMetaPure.test.cpp`, `tests/Lua/LuaBindings.test.cpp` updated to the new signatures + asserting on `r.error().Kind` / `.Detail` where they previously asserted on the `outError` string. **test-author** wires these per slice. Zero-behaviour-change is the gate: same pass/fail per scripted reply.
- **Bucket E (ImGui Test Engine)**: N/A — pure refactor, no new interactive surface (Slices 1–7 leave UI untouched). If Slices 8–9 proceed, the field-edit / annotate-comment flows already have coverage; re-run, no new wiring.
- **Bash-driver scenario / screenshot / sanitizer**: **Bucket D (sanitizer)** — run the ASan/UBSan build on the largest slice (Slice 4) to confirm no use-after-move on extracted `Result` payloads. **test-author** owns.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) per slice.
- **Doc validation (blocks plan-doc PRs)**: `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs`**: run before finalising; sharpen `Result`-vs-bare-`TrackerError` terminology against `Source/Core/src/Tracker/CONTEXT.md` glossary; record outcome.
- **Manual residue**: none — all buckets automated.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray "deferred-as-current" refs to migrated symbols and revise.

- `FetchIssues` streaming/pointer-out path (landmine L4) — no-action this plan; separate sync-engine effort.
- `ITrackerConnectivity` — no `outError` methods, nothing to migrate.
- `AppController` public wrapper + UI/Command caller flip (Slices 8–9) — follow-on phase; only if user confirms the FULL stack (not just the virtual interface) is wanted.
- Non-tracker `outError` sites (Whisper / P4 / config / texture cache) — separate hardening item.

## Implementation log
*Slices 1–7 shipped (the FULL `ITrackerBackend` virtual-interface migration); parent hardening #21b SHIPPED via #1037 (`5eefb9c2`). Slices 8–9 (public-API flip) are scoped-but-not-executed follow-on, owned by build-quality-velocity-hardening.*

- `775b462f` (#1020) · Slice 1 · `GitHubClientHelpers` validators/parsers → `Result<T>` (default-E `std::string`): `IsValidGitHubBaseUrl` / `ParseIso8601ToUnixSec` / `BuildGitHubCreatePayload`.
- `09fa95f4` (#1022) · Slice 2 · FieldCatalog virtuals → `Result<T, TrackerError>`: `FetchFieldCatalog` / `FetchIssueEditMeta` / `FetchProjectComponents` (+ FIX-1 2xx guard, FIX-3 Unknown-wrap deferral).
- `a406c696` (#1023) · Slice 3 · Mutations payload builders → `Result<nlohmann::json, TrackerError>`: `BuildFieldPayload` / `BuildCreatePayload` / `BuildUpdatePayload`.
- `1cc98f9b` (#1029) · Slice 4 · Mutations void-payload writes → bare `TrackerError`: `UpdateIssueFields` / `UpdateField` / `AddIssueToSprint`.
- `f62105cd` (#1030) · Slice 5 · Mutations create/attach → `Result<T, TrackerError>`: `CreateIssue` / `AttachFilesToIssue` (L3 — per-file failures are the Ok payload).
- `d5eed664` (#1033) · Slice 6 · Reader `FetchIssuesForKeys` → `Result<std::vector<CachedTicket>, TrackerError>` (incl. the `GitHubIssueSearch::FetchIssuesForKeysViaRestApi` helper).
- `8a02d450` (#1035) · Slice 7 · Collaboration virtuals → `Result<T>` reads / bare `TrackerError` writes (9 methods) + the `TrackerIssueVotes` struct.

## Deviations from plan
- **Slices 8–9 (AppController public-API flip) — explicit FOLLOW-ON, out of this plan's delivered scope:** scoped-but-not-executed; ownership handed to **build-quality-velocity-hardening**. Slices 1–7 stand as the delivered #21b goal.
- **Slice 7 — Collaboration (7a reads + 7b writes shipped together as one PR):** the plan flagged a 7a/7b split "if over ceiling"; the combined diff is ~8 files (well under), so both ship in one "Collaboration" PR per the one-PR-per-feature batching rule. `FetchIssueComments` migrated **decl-only** (L5 — no override, no caller; Jira uses the internal `JiraFetchIssueCommentsPages` helper, untouched). `FetchIssueVotes`' four out-params (`outVoters` + `int* outVoteCount` + `bool* outHasVoted` + `bool* outVotersInResponse`) collapsed into the `TrackerIssueVotes` Ok struct; the `AppController::FetchIssueVotes` wrapper unpacks the struct back into its existing out-pointer signature (zero-inits them up-front to match the old virtual's entry-zeroing) so UI callers (`TrackerGridFieldDisplay`, `BuiltinCommands_Users`) are untouched. All Jira HTTP failure branches carry the FIX-1 2xx guard before `TrackerErrorFromHttpStatus`. `EnsureTrackerAuthConfig`/missing-creds fails → `Auth`; empty-key/precondition → `InvalidRequest`; parse/missing-key → `Parse`. The 8 `AppController` wrappers translate the Result/`TrackerError` back to their existing `bool + outError`(+ out-param) public signatures, so **all UI/command callers stay untouched** (the public-wrapper flip is Slices 8–9). GitHub/Plane/all fixtures use the new interface defaults (Collaboration is Jira-only).
- **Slice 6 — partial-fetch tickets dropped on `Err` + defensive 2xx guards:** `FetchIssuesForKeys` accumulates tickets across pages/keys; on a mid-stream failure the old code returned `false` and the callers (`AppController` prefetch, `OfflineQueueService` conflict re-fetch) discarded the partial `outTickets`. The migration returns `Err` and drops the partial accumulation identically (the local vector isn't moved into an Ok) — zero behaviour change. The Jira search `!= 200` branch and the GitHub `FetchIssuesForKeysViaRestApi` `!= 200` branch both carry the **FIX-1 2xx guard** before `TrackerErrorFromHttpStatus` (defensive — these endpoints return 200 on success, but a 2xx-other must not collapse to a dropped `Ok()` inside an `Err(...)`). Plane's path surfaces only `summary.FetchError` (a string, no HTTP status) → wrapped `Unknown` with Detail verbatim + a TODO (same precedent as Slice 4 Plane). Fixture `loadError_` → `InvalidRequest`. Both callers read `.Detail` into their existing `err`/`fetchErr` string, so the `IsTrackerTransportErrorText` archive-vs-retry decision is byte-identical.
- **Slice 5 — `AttachFilesToIssue` hard-fail is still swallowed by the caller (pre-existing behaviour preserved):** per L3, per-file failures become the Ok payload and only hard failures (auth / empty key / Plane-unsupported) are `Err`. The sole caller `IssueCreatePipeline::ApplyPostIssueAttachmentStep` historically **ignored** the old `bool`/`outError` and read only `result.AttachmentFailures`; the old hard-fail path returned `false` with an *empty* failures list, so a hard attach failure was silently dropped. The migration preserves that exactly (on `Err`, `AttachmentFailures` stays empty → no warn) rather than newly surfacing it — zero behaviour change. Follow-up (Slices 8–9 or later) could map `Err` to a synthetic failure entry so hard attach failures are reported; flagged, not done here.
- **Slice 5 — Plane `CreateIssue` "created, key unknown" → `Ok("")` not `Err`:** Plane's post-2xx parse-fail path historically returned an empty key with *no* error ("created, key unknown"), and the caller treats any empty key as failure (`"Create failed."`). Migrated to `Ok(std::string())` (not `Err`) so the caller's existing empty-key branch fires identically. Jira/GitHub never return `Ok("")` (success always carries a non-empty key; all failures are `Err`). HTTP-failure branches in all three `CreateIssue` impls use the FIX-1 2xx guard before `TrackerErrorFromHttpStatus`; Plane's `ResolvePlaneProject`-fail keeps the Slice-4 `Unknown`-wrap + TODO.
- **Slice 4 — Unknown-wrap of kept-bool internal write helpers (known, deferred):** the migrated void-payload virtuals delegate to private/non-virtual helpers that stay `bool + outError` per landmines L1/L2 — Jira `UpdateIssueFieldsViaTransition`/`ViaPut` (behind `UpdateIssueFields`), Jira `AddIssueToSprint(cfg,…)` (behind the public `AddIssueToSprint`), and Plane `ResolvePlaneProject` (behind `UpdateIssueFields`). Their string error is wrapped as `TrackerErrorUnknown` (Detail preserved **verbatim** → caller status-text parsing via `ErrorTextContainsHttpStatus` / `IsTrackerTransportErrorText` is unaffected → zero behaviour change), which loses HTTP-status classification. `// TODO(#21b later slice)` markers left at all three sites; re-threading lands when an `IsRetryable()` consumer arrives (same precedent as Slice 2 FIX 3). Plane's **direct-HTTP** `UpdateIssueFields` failure branch DOES classify via `TrackerErrorFromHttpStatus`, guarded by the FIX-1 2xx check (a 201/202/206 reaching the `!= 200 && != 204` branch → `TrackerErrorUnknown(detail, status)`, never a dropped `Ok()`).
- **Slice 2 — 2xx-non-200 guard (FIX 1):** `TrackerErrorFromHttpStatus` returns `Ok()` (Kind==None, detail discarded) for any status in [200,300). Two migrated failure branches (`JiraClient::FetchIssueEditMeta` non-200 branch, `JiraClient::FetchProjectComponents` per-project non-200 branch) gated on `status_code != 200`, so a 201/202/204 reaching the failure path produced a contradictory `Err` with an empty `.Detail` — the user-visible message vanished. Fixed: both branches now guard `status >= 200 && status < 300` → `TrackerErrorUnknown(detail, status)` (preserving the verbatim detail string), falling through to `TrackerErrorFromHttpStatus` only for genuine non-2xx. Learning: any `status != 200` (rather than `< 200 || >= 300`) failure branch must not feed `TrackerErrorFromHttpStatus` without this guard.
- **Slice 2 — classification-lossy Unknown wrap (FIX 3, known, deferred):** the Jira virtual `FetchFieldCatalog` delegated-helper unwrap (`TrackerFieldCatalog.cpp`) and the Plane `ResolvePlaneProject`-fail unwrap (`PlaneFieldCatalog.cpp`) collapse the inner helper's error to `TrackerErrorUnknown`, losing HTTP-status classification (a real 5xx becomes Unknown → `IsRetryable()` false). No consumer reads `.Kind` this slice; `// TODO(#21b later slice)` markers left at both sites. Re-threading status from the inner helper lands in a later slice when `IsRetryable()` consumers arrive.

## Verification (actual)
- **Slice 7** (Collaboration — 5 reads → `Result<payload,TrackerError>`, 4 writes → bare `TrackerError`, + `TrackerIssueVotes` struct): dual-target build green (`SmatchetStandalone` + `SmatchetCore_DX12`, `ninja-iter-msvc`); `SmatchetTests` green (`ninja-test-msvc`) incl. the added `ITrackerCollaboration` migrated-shape cases (overridden read → Ok payload incl. votes-struct field collapse, overridden write → Ok `TrackerError`, non-overridden read/write defaults → `InvalidRequest` Err). Migrated interface + `JiraClient` (the only Collaboration backend) + the 8 `AppController` wrappers; UI/command callers untouched. Lint + clang-format clean. Worktree reused. Zero behaviour change. **This completes the FULL `ITrackerBackend` virtual-interface migration (Slices 1–7).**
- **Slice 6** (`FetchIssuesForKeys` → `Result<std::vector<CachedTicket>,TrackerError>`): dual-target build green (`SmatchetStandalone` + `SmatchetCore_DX12`, `ninja-iter-msvc`); `SmatchetTests` green (`ninja-test-msvc`) incl. the added reader-contract cases (Ok-tickets / Err-detail-transport-text). Migrated the interface + all 5 backends (incl. the `GitHubIssueSearch::FetchIssuesForKeysViaRestApi` free-function helper) + both prod fixtures + `FakeTrackerClient` + the 2 callers; the `SetFetchIssuesForKeysResult` scripting API stayed unchanged (translates internally → existing OfflineQueue runtime tests untouched). Lint + clang-format clean. Worktree reused. Zero behaviour change.
- **Slice 5** (`CreateIssue` → `Result<std::string,TrackerError>`, `AttachFilesToIssue` → `Result<vector<pair>,TrackerError>`): dual-target build green (`SmatchetStandalone` + `SmatchetCore_DX12`, `ninja-iter-msvc`); `SmatchetTests` green (`ninja-test-msvc`) incl. the added Slice-5 contract cases (CreateIssue Ok-key/Err-detail; AttachFiles Ok-empty / Ok-partial-failures / Err-hard-fail) + the migrated `JiraFakeTrackerFixture.test.cpp` CreateIssue direct call. Lint + clang-format clean. Worktree **reused** from Slice 4 (warm build dirs → no dep reconfigure). Zero behaviour change: `IssueCreatePipeline` create/attach surfaces (`IssueCreateResult.IssueKey` / `.Error` / `.AttachmentFailures`) are byte-identical.
- **Slice 4** (`UpdateIssueFields`, `UpdateField`, `AddIssueToSprint` → bare `TrackerError`): dual-target build green — `SmatchetStandalone` (`Smatchet.exe`) + `SmatchetCore_DX12` (`SmatchetCore_DX12.lib`) via `ninja-iter-msvc`. `SmatchetTests` green via `ninja-test-msvc` (incl. the added Slice-4 void-payload contract cases in `IssueCreatePipelineIntegration.test.cpp` + the migrated `JiraFakeTrackerFixture.test.cpp` direct call). Lint: `test-lint-rules.sh --diff origin/develop` PASS (dup WARNs are pre-existing header-signature similarity, calibration-phase non-blocking). clang-format clean. Zero behaviour change: every caller still consumes `.Detail` verbatim, so `ErrorTextContainsHttpStatus` (editmeta-400 retry) and `IsTrackerTransportErrorText` (offline-replay archive-vs-retry) decisions are byte-identical.
- **Slice 3** (`BuildFieldPayload` / `BuildCreatePayload` / `BuildUpdatePayload` → `Result<nlohmann::json, TrackerError>`) *(reconstructed from the `a406c696`/#1023 merge record)*: dual-target build green (`SmatchetStandalone` + `SmatchetCore_DX12`); `SmatchetTests` **1479/1479**. Lint `test-lint-rules.sh --diff origin/develop` PASS; clang-format clean; code-review clean (0 Critical/High/Medium).
- **Slice 2** (`FetchFieldCatalog` / `FetchIssueEditMeta` / `FetchProjectComponents` → `Result<T, TrackerError>` + `TrackerProjectComponents` struct) *(reconstructed from the `09fa95f4`/#1022 merge record)*: dual-target build green; `SmatchetTests` **1479/1479**. Lint PASS; clang-format clean; code-review clean (1 Medium found + fixed; no Critical/High). The FIX-1 2xx guard + FIX-3 Unknown-wrap deferral originate here (see § Deviations).
- **Slice 1** (`GitHubClientHelpers` validators/parsers → `Result<T>` default-E) *(reconstructed from the `775b462f`/#1020 merge record)*: dual-target build green; `SmatchetTests` **1475/1475**. Lint PASS; clang-format clean. First slice — validated the `Result` type + strict-zone lint mechanics before any virtual dispatch.
