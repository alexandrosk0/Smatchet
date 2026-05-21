# Plan — GitHub as a third tracker backend

> **Slug**: `github-tracker-backend`
>
> **Mandatory rules cross-link**: see [`AGENTS.md`](../../AGENTS.md) § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Smatchet today ships two grid-backing tracker backends — Jira and Plane — wired via `ITrackerClient` and `DefaultTrackerBackendFactory`. A third surface, GitHub, already exists in-tree as [`GitHubClient`](../../Source_Core/include/GitHubClient.h) but is currently **triage-only**: it powers the agentic-flow pipeline (CodeRabbit + CI react loop, `handoff-implementer`, `pr-iterator`, the T7 scheduled-poll worker) and stubs out every grid-relevant `ITrackerClient` virtual with the documented "not supported on GitHub backend yet" sentinel.

User ask: let users pick **GitHub** as the active tracker so issues from a configured `owner/repo` populate the grid, sync into the SQLite cache, and support inline field-edit + create-issue flows the same way Jira/Plane do today.

Critical constraint from the user: **no duplication of the existing triage flow**. The agentic-flow's CommentAdd / LabelAdd / LabelRemove / AssigneeSet / StateTransition / PR-thread / check-run / GraphQL-resolve methods on `GitHubClient` must stay the single source of truth for GitHub writes. The tracker role must **route** through those primitives, not parallel them.

Cross-link: [`docs/design/agentic-flow-implementation.md`](agentic-flow-implementation.md) (the triage-side contract this plan must not regress).

## Approach

Extend the existing `GitHubClient` to fill in its stubbed `ITrackerClient` virtuals. No second class, no `GitHubTrackerClient` parallel. Single instance per process, shared between the agentic-flow paths (`AppController::EnsureAgenticGithubClient`) and the tracker-role path (`DefaultTrackerBackendFactory::Create("github")`) via promotion of the existing `agenticGithubClient_` into a tracker-aware shared owner.

Three changes shape the diff:

1. **De-gate the TU** — drop `#if defined(SMATCHET_WITH_AGENTIC)` around the `GitHubClient` source list in `CMakeLists.txt` and the `GitHubPat` config field. The triage code path stays opt-in via runtime config (empty PAT → unsupported sentinel), but the symbol must exist in tracker-only builds. The build-time gate was the cheapest way to keep agentic code out of vanilla builds when the client landed; now that the same class serves both roles, the gate is the wrong axis.
2. **Implement the stubbed virtuals** — `FetchIssues`, `FetchIssuesForKeys`, `ProbeReachability`, `BuildBrowseUrl`, `ExtractProjectFromQuery`, `ListProjects`, `FetchFieldCatalog` (static, no API), `ResolveDisplayValue`, `UpdateIssueFields`, `UpdateField`, `BuildFieldPayload`, `BuildCreatePayload`, `CreateIssue`. The four write virtuals **dispatch internally** to the existing `CommentAdd` / `LabelAdd` / `LabelRemove` / `AssigneeSet` / `StateTransition` primitives plus one new shared `PatchIssue` helper for `title` / `body` / `milestone` (the three GitHub fields that share the `PATCH /repos/{o}/{r}/issues/{n}` endpoint). No new HTTP call paths for state / labels / assignees.
3. **Wire the factory + config** — extend `DefaultTrackerBackendFactory::Create` with a `"github"` branch; add `GitHubOwner`, `GitHubRepo`, `GitHubBaseUrl` to `TrackerConfig` (PAT already there); extend `SmatchetPreferencesUi` with the GitHub profile group. Schema bump deferred until the whole rollout verifies end-to-end (per [`AGENTS.md`](../../AGENTS.md) § Project rules § Schema-version bumps).

The non-obvious trade-off: GitHub Issues has very few native fields (state, labels, assignees, milestone, title, body). Custom fields live on **Projects v2** behind GraphQL, which is a separate effort. Phase 1 ships the native-fields tracker only; Projects-v2 fields land as a follow-up plan once the native flow is shipped and validated — keeps the diff scoped and the first slice testable.

## Files to modify

Numbered list; grouped by subsystem.

**Backend client — extend existing TU**

1. [`Source_Core/include/GitHubClient.h`](../../Source_Core/include/GitHubClient.h:35) — drop the "only `FetchIssueComments` is real this slice" comment; un-stub virtual declarations; add `PatchIssue` private helper.
2. [`Source_Core/src/GitHubClient.cpp`](../../Source_Core/src/GitHubClient.cpp:107) — replace stub bodies for `ProbeReachability` / `FetchIssues` / `FetchIssuesForKeys` / `UpdateIssueFields` / `UpdateField` / `BuildFieldPayload` / `ResolveDisplayValue`; add `BuildBrowseUrl`, `ExtractProjectFromQuery`, `ListProjects`, `FetchFieldCatalog`, `BuildCreatePayload`, `CreateIssue` overrides; add private `PatchIssue` helper.
3. **New file** [`Source_Core/src/GitHubIssueSearch.cpp`](../../Source_Core/src/) — extract `FetchIssues` paginated `since=` loop + cursor logic into its own TU (mirrors the `JiraIssueSearch.cpp` / `PlaneIssueSearch.cpp` split). Keeps `GitHubClient.cpp` from ballooning past ~800 LOC.
4. **New file** [`Source_Core/src/GitHubFieldCatalog.cpp`](../../Source_Core/src/) — static catalog builder (state / labels / assignees / milestone / title / body) + `ResolveDisplayValue`. Mirrors `PlaneFieldCatalog.cpp`.
5. [`Source_Core/include/GitHubClientHelpers.h`](../../Source_Core/include/GitHubClientHelpers.h) — add `BuildIssuePatchSuffix` (shared by state-transition + new `PatchIssue`) and `BuildIssuesListSuffix` (paginated list builder, mirrors `ListOpenIssuesForRepo`'s URL composition).
6. [`Source_Core/src/GitHubClientHelpers.cpp`](../../Source_Core/src/GitHubClientHelpers.cpp) — implementations + tests.

**Factory + config**

7. [`Source_Core/src/DefaultTrackerBackendFactory.cpp`](../../Source_Core/src/DefaultTrackerBackendFactory.cpp:7) — add `if (lower == "github") return std::make_unique<GitHubClient>(cfg.GitHubBaseUrl, cfg.GitHubPat);`. Factory does not own the agentic client — `AppController` continues to hold the shared instance; the factory only constructs the tracker-role owner. Single-instance discipline: see § Risks § Two `GitHubClient` instances.
8. [`Source_Core/include/ConfigManager.h`](../../Source_Core/include/ConfigManager.h:222) — un-gate `GitHubPat` (drop `#if SMATCHET_WITH_AGENTIC`); add `GitHubBaseUrl`, `GitHubOwner`, `GitHubRepo`. `AgenticPollSource` / `AgenticPollQuery` stay separate — the agentic poll and tracker query are independent (a user may track `org/repo-A` while polling `org/repo-B` for proposals).
9. [`Source_Core/src/ConfigManager.cpp`](../../Source_Core/src/ConfigManager.cpp) — Load / Save the three new fields; legacy-config migration.
10. [`Source_Core/src/SmatchetPreferencesUi.cpp`](../../Source_Core/src/SmatchetPreferencesUi.cpp) — add the GitHub profile group (PAT, base-URL, owner, repo) under the existing Tracker section.

**Tracker-role wire-in**

11. [`Source_Core/src/AppController.cpp`](../../Source_Core/src/AppController.cpp:1655) — promote `agenticGithubClient_` to `sharedGithubClient_`; both `EnsureAgenticGithubClient` and the factory-created tracker share the same `unique_ptr`. New helper `AppController::GetGithubClient()` returns the live instance; factory's `Create("github")` calls it through a `TrackerBackendFactory` overload that takes an `AppController&` context. (Alternative: a process-singleton; rejected — singletons are testing-hostile.)
12. [`Source_Core/include/AppController.h`](../../Source_Core/include/AppController.h) — rename member; add accessor.

**Tests (Bucket A — pure-logic ctest)**

13. [`tests/Source_Core/GitHubClient_FieldCatalog.test.cpp`](../../tests/Source_Core/) — new. Static-catalog shape: 6 fields, correct types, allowed values for `state`.
14. [`tests/Source_Core/GitHubClient_UpdateField.test.cpp`](../../tests/Source_Core/) — new. Field-router dispatch: assert that `UpdateField("state", ["closed"])` goes through `StateTransition`, `UpdateField("labels", ["bug","p0"])` issues a `LabelAdd` per new label + `LabelRemove` per removed label, etc. Mock the HTTP layer per the existing `GitHubClient_GraphQL.test.cpp` pattern.
15. [`tests/Source_Core/GitHubClientHelpers.test.cpp`](../../tests/Source_Core/GitHubClientHelpers.test.cpp) — extend with `BuildIssuePatchSuffix`, `BuildIssuesListSuffix`, and `ExtractProjectFromQuery` (parse `repo:owner/name` → `owner/name`).
16. [`tests/Source_Core/GitHubFieldCatalog.test.cpp`](../../tests/Source_Core/) — new. `ResolveDisplayValue` mapping for assignees-by-id vs labels-by-name.

**Build glue**

17. [`CMakeLists.txt`](../../CMakeLists.txt) — drop `SMATCHET_WITH_AGENTIC` from the `GitHubClient*.cpp` source-list condition; gate the agentic-only test TUs separately if they reference triage-only helpers.

## Existing utilities reused

The anti-duplication contract lives in this section.

- `GitHubClient::CommentAdd` + `LabelAdd` + `LabelRemove` + `AssigneeSet` + `StateTransition` — [GitHubClient.h:134-168](../../Source_Core/include/GitHubClient.h:134) — the tracker-role `UpdateField` is a router over these five; no new HTTP path for state/labels/assignees/comments.
- `GitHubClient::ListOpenIssuesForRepo` cursor pattern — [GitHubClient.h:112](../../Source_Core/include/GitHubClient.h:112) — `FetchIssues` reuses the `since=<iso>` cursor + 30-per-page cap. Promotes the inline URL builder to `GitHubClientHelpers::BuildIssuesListSuffix` so both call sites share the parameter encoding.
- `GitHubClient::FetchIssueComments` + `FetchIssueBody` — already implement the read-path PAT-presence + parse + redacted-error compose pattern. New `FetchIssues` follows it byte-for-byte; no new error-compose shape.
- `MakeGitHubAuthHeaders` (anonymous-namespace helper) + `ComposeHttpErrorString` + `RedactForLog` — [GitHubClient.cpp:38-69](../../Source_Core/src/GitHubClient.cpp:38) — every new HTTP call uses these. Per [`docs/design/agentic-flow-implementation.md`](agentic-flow-implementation.md) § Decisions locked #2, the inline-bearer pattern stays; no promotion to `BuildTrackerHeaders` until a second bearer-auth backend lands. GitHub is that backend — but only for **tracker** writes; the triage path's audit-source `"github_client"` stays unchanged.
- `GitHubClientHelpers::ParseGitHubIssueKey` + `BuildIssue*Suffix` family — [GitHubClientHelpers.h](../../Source_Core/include/GitHubClientHelpers.h) — every new write method parses keys through the same helper. Key shape stays `owner/repo#N` (already canonical, already documented in `ITrackerClient::FetchIssueComments` doxygen).
- `BackendAuditTrail::AppendBegin` / `AppendResult` with `source="github_client"` — [GitHubClient.cpp:21](../../Source_Core/src/GitHubClient.cpp:21) — every new write goes through the same audit-trail shape. Triage-flow audit-consumers (filter on `source="github_client"`) keep working unchanged.
- `CachedTicket` + `LocalCacheManager` — pure-data shape already used by Jira / Plane `FetchIssues`. `FetchIssues` produces the same struct; no new cache-row schema.
- `TrackerFieldCatalogResult` + `TrackerField` — [`TrackerFieldSchema.h`](../../Source_Core/include/TrackerFieldSchema.h) — static catalog builds the same struct Jira/Plane fill from API responses.
- `AppController::EnsureAgenticGithubClient` lazy + `std::call_once` ownership — [AppController.cpp:1655](../../Source_Core/src/AppController.cpp:1655) — renamed + reused. No second instance.
- `smatchet::ai::pure::RedactProviderErrorBody` — shared with the AI provider error path; defense-in-depth against PAT echo. New calls keep using it.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: tracker fetch + field edits are off-UI by existing `ITrackerClient` contract — Jira/Plane already run on worker threads, GitHub follows the same call sites. Steady-state UI work unaffected. Grid rendering with GitHub-sourced rows shares the same `TicketGridModel` pipeline as Jira/Plane; perf-equivalent.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: every new HTTP method preserves the existing 5s connect / 15s overall timeout pattern (`kGitHubConnectTimeoutMs` / `kGitHubOverallTimeoutMs`). Call sites stay on worker threads — `TicketSyncService`, the field-edit pipeline, the issue-create pipeline already dispatch tracker calls off the UI thread. Field-edit UX: existing optimistic-write + spinner pattern (used by Jira/Plane field editors) covers GitHub identically.
- **Pillar 3 (never crash)**: every new method follows the existing GitHubClient error-handling discipline — PAT-presence check first, parse-fail returns `false` + `outError`, no raw `new` / `delete`, all heap via `std::unique_ptr`. JSON parsing wrapped in `try` / `catch (json::parse_error&)` per existing pattern. Sanitizer build runs via `ninja-test-msys2` per project rules.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no new UI widgets beyond the Preferences profile group, which reuses existing `SmatchetPreferencesUi` widget conventions (keyboard-reachable, scales with `FontGlobalScale`, palette from theme). N/A as a regression risk.

## Perf-review-system gates

Per [`docs/design/pillar-1-2-perf-review-system.md`](pillar-1-2-perf-review-system.md). Diff touches `Source_Core/` — gates apply.

1. **PR-fast CI** — scenario subset map ([`agents/perf-gatekeeper.md`](../../agents/perf-gatekeeper.md) § Curated diff → scenario map). New tracker backend exercises `tracker_sync` + `grid_render` scenarios most directly. If those aren't in `scripts/dev/perf-pr-fast-set.json`, the gate falls back to the broader tracker scenarios — verify before opening PR.
2. **Pillar 2 static scanner** — no new sync-I/O reachable from `ImGui::*`. All HTTP / parse work stays on worker threads via existing tracker dispatch. `/* PILLAR2_WORKER_ONLY */` annotation not required (no new UI-thread entry points).
3. **Dispatcher drain** — no new `MainThreadDispatcher::Drain()` touch. Worker-thread results post back through the existing tracker dispatch path Jira/Plane use.
4. **Visible-cue bucket-E harness** — no new sync stalls > 100 ms; existing spinner / progress widgets cover the new code paths via shared call sites.
5. **Marker inventory** — no new `SMATCHET_UI_PERF_SCOPE` markers planned. If the implementer adds any for tracker-fetch hot paths, regen `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push local check**: `bash scripts/dev/perf-run.sh tracker_sync` + `python scripts/dev/perf-compare.py docs/perf/baselines/tracker_sync.dev.json build/perf-runs/tracker_sync-<ts>.json --markdown-only`. `MISSING_BASELINE` is acceptable here — GitHub-sourced tracker_sync has no prior baseline; CI auto-bootstraps on first run.

**Override**: `perf-out-of-band` PR label per [`AGENTS.md`](../../AGENTS.md) § Merge gates — not expected to fire (no perf regression intent).

## Risks / non-goals

**Risks**:

- **Two `GitHubClient` instances** — naive wire-in (factory builds its own, AppController keeps `agenticGithubClient_` for triage) creates two PAT-holding clients per process. Mitigation: promote `agenticGithubClient_` to `sharedGithubClient_`; factory's `Create("github")` returns a `unique_ptr` to a forwarding shell that delegates to `AppController::GetGithubClient()`. Tracker subsystems already accept a `unique_ptr<ITrackerClient>`; the forwarding shell is the cheapest adapter. Alternative considered: change `TrackerBackendFactory::Create` to return a non-owning pointer — rejected because Jira/Plane callers expect ownership.
- **`SMATCHET_WITH_AGENTIC` un-gating** — dropping the build gate adds the triage code path (PR/check-run/GraphQL) to the standalone binary even when the user doesn't use it. Binary-size impact: ~150 LOC of additional cpr call sites + helpers. Accepted — same trade Plane made when it landed. The runtime PAT-empty short-circuit means the code is reachable but never executes without configuration.
- **GitHub PATs vs OAuth** — current design keeps PAT-only. Mitigation: `TrackerConfig::GitHubBaseUrl` lets the user point at GitHub Enterprise or a future PAT-issuing proxy. OAuth is a follow-up plan; not blocking.
- **Custom fields on Projects v2** — phase 1 ships native fields only. Users who want sprint/iteration/custom-status fields can't get them yet. Mitigation: § Out of scope flags the follow-up plan explicitly. Field catalog's static-builder leaves room to merge in a GraphQL-fetched Projects-v2 catalog later without breaking the native-field shape.
- **Rate limits** — GitHub's REST API caps unauthenticated calls at 60/hr, PAT-authed at 5000/hr. `FetchIssues` paginating 30-per-page on a 1000-issue repo = 34 requests; comfortable under the cap. Mitigation: pagination cap at 30 items per `ListOpenIssuesForRepo` already in place; `FetchIssues` follows the same cap with a `?per_page=100` upgrade once auth is confirmed (saves ~3x requests). Rate-limit hit returns 403; existing `ComposeHttpErrorString` surfaces it.

**Non-goals**:

- **Projects v2 custom fields via GraphQL** — separate plan.
- **GitHub Issues comment writes from the tracker grid** — the tracker's `AddIssueCommentPlain` virtual maps to existing `CommentAdd`, but the **comment UI** (the rich-text editor surface) is a separate UX track and stays Jira/Plane-only this phase.
- **PR-as-issue tracking** — GitHub PRs are a superset of Issues. Tracker stays issues-only; PR objects continue to live in the agentic-flow's PR-watcher surface, not the grid.
- **GitHub Enterprise OAuth / SSO** — PAT only this phase.
- **Migration tooling** — no Jira-to-GitHub or Plane-to-GitHub issue migration. Users configure GitHub as a fresh tracker.

## Verification

Per [`AGENTS.md`](../../AGENTS.md) § Verification automation — zero manual steps.

- **Bucket A (pure-logic ctest, `test-rig`)**:
  - `tests/Source_Core/GitHubClient_FieldCatalog.test.cpp` — static catalog shape: 6 fields, allowed-value sets.
  - `tests/Source_Core/GitHubClient_UpdateField.test.cpp` — `UpdateField` router dispatch (HTTP mock per existing `GitHubClient_GraphQL.test.cpp` pattern). One sub-test per field-id → primitive mapping. Assert that no field-id path makes a raw HTTP call outside the primitive set.
  - `tests/Source_Core/GitHubFieldCatalog.test.cpp` — `ResolveDisplayValue` for assignees + labels.
  - `tests/Source_Core/GitHubClientHelpers.test.cpp` (extend) — `BuildIssuePatchSuffix`, `BuildIssuesListSuffix`, `ExtractProjectFromQuery` round-trips.
- **Bucket E (ImGui Test Engine)**: extend the existing tracker-switching scenario (if present at `tests/ui/`) to flip between Jira / Plane / GitHub and assert grid rows populate. If no such scenario exists, this is a follow-up `docs/backlog/agent-self-improvement/test.md` entry.
- **Bash-driver scenario**: `scripts/dev/test-github-tracker.sh` (new) — spawn standalone with `GITHUB_PAT_TEST` env, run `tracker.set github`, run `tracker.sync`, assert ≥ 1 row in the SQLite cache. Skip when env unset.
- **Build gate**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — must pass with `SMATCHET_WITH_AGENTIC=OFF` (no-agentic build still compiles `GitHubClient` per the un-gate).
- **Sanitizer gate**: `cmake --build --preset ninja-test-msys2` — ASan/UBSan clean.
- **Perf gate**: `bash scripts/dev/perf-run.sh tracker_sync` — see § Perf-review-system gates.
- **Manual residue**: GitHub PAT must be configured at least once interactively in Preferences for end-to-end smoke; this is the same gate Jira/Plane have today (no PAT = no live tracker, by design). Not a deferred-automation entry — the alternative is shipping a test PAT in CI, which is a security non-starter.

## Out of scope (flagged, not designed)

- **Projects v2 custom fields** — follow-up plan `docs/design/github-projects-v2-fields.md`. Drops in by extending `FetchFieldCatalog` to merge a GraphQL `node(id: <project>) { fields { ... } }` result into the static native-field catalog. No breaking change to phase 1.
- **GitHub Apps + OAuth** — follow-up plan; PATs cover the immediate need.
- **Repo-multi-select on a single tracker profile** — phase 1 is one `owner/repo` per tracker profile. Multi-repo views via JQL-equivalent `repo:` clauses in `ExtractProjectFromQuery` could land later; no design here.
- **GitHub-side audit-trail consumer** — the existing `BackendAuditTrail` already captures all writes; no new consumer needed. Triage-flow audit-row joins on `source="github_client"` keep working unchanged.
- **Comment-thread UI on the grid** — tracker-grid comment UI is Jira/Plane-only this phase. GitHub comment reads go through the existing agentic-flow read paths.

## Implementation log
*(populated post-ship per [`AGENTS.md`](../../AGENTS.md) § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
