# Plan — GitHub as a third tracker backend (+ triage tracker-agnostic refactor)

> **Slug**: `github-tracker-backend`
>
> **Mandatory rules cross-link**: see [`AGENTS.md`](../../AGENTS.md) § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Smatchet today ships two grid-backing tracker backends — Jira and Plane — wired via `ITrackerClient` and `DefaultTrackerBackendFactory`. A third surface, GitHub, already exists in-tree as [`GitHubClient`](../../Source_Core/include/GitHubClient.h) but is currently **triage-only**: it powers the agentic-flow pipeline (CodeRabbit + CI react loop, `handoff-implementer`, `pr-iterator`, the T7 scheduled-poll worker) and stubs out every grid-relevant `ITrackerClient` virtual with the documented "not supported on GitHub backend yet" sentinel.

Triage's own seam — [`IGitHubReadClient`](../../Source_Core/include/AgenticTriageController.h:47) — is **GitHub-shaped**: `ListOpenIssuesForRepo(owner, repo, ...)` takes a GitHub URL fragment, not an abstract query. AppController binds triage write lambdas (`CommentAdd` / `LabelAdd` / `AssigneeSet` / `StateTransition`) **directly** to `GitHubClient::*` methods. That binding is the duplication this plan eliminates.

User ask, two parts:

1. Let users pick **GitHub** as the active tracker so issues populate the grid, sync into the SQLite cache, and support field-edit + create-issue flows the same way Jira/Plane do.
2. **Refactor triage to use `ITrackerClient`** so the agentic-flow's discovery + comment + label + state-transition + clarification paths work against Jira and Plane sources too — not just GitHub.

Critical constraint: **the existing triage flow must not break**. GitHub-only capabilities (PR threads, check-runs, GraphQL review-thread resolve) stay typed on `GitHubClient`; only the issue-side surface generalizes.

Cross-link: [`docs/design/agentic-flow-implementation.md`](agentic-flow-implementation.md) (the triage-side contract); [`docs/design/agentic-triage-flow.md`](agentic-triage-flow.md) (current GitHub-shaped triage architecture); [`docs/design/coderabbit-react-loop.md`](coderabbit-react-loop.md) + [`docs/design/agentic-coding-handoff.md`](agentic-coding-handoff.md) (PR-side flows that stay GitHub-only).

## Approach

Two parallel work-streams, sequenced so the refactor lands first:

**A. Triage tracker-agnostic refactor** (lands first; does not depend on GitHub-as-tracker).

A.1 Promote `FetchIssueBody` to `ITrackerClient` as a new virtual (default sentinel). Jira + Plane fill it in; GitHubClient already has it.

A.2 Add `ITrackerClient::ListIssueKeysUpdatedSince(const TrackerConfig& cfg, const std::string& query, std::int64_t sinceUnixSec, std::vector<std::string>& outKeys, std::string& outError)` virtual (default sentinel). Backend-specific query shape: `owner/repo` for GitHub, JQL for Jira, project-UUID-filter for Plane. Returns canonical issue keys (`owner/repo#N`, `PROJ-42`, `<plane-uuid>`).

A.3 Reshape [`IGitHubReadClient`](../../Source_Core/include/AgenticTriageController.h:47) → `ITrackerTriageSource` (rename + reshape; not a parallel interface). Methods reduce to three thin pass-throughs over `ITrackerClient` virtuals — the seam still exists for doctest substitutability, but the production adapter wraps any `ITrackerClient`, not only `GitHubClient`. `ListOpenIssuesForRepo(owner, repo, ...)` collapses to `ListIssueKeysUpdatedSince(query, ...)`.

A.4 Refactor triage write lambdas in [`AppController.cpp`](../../Source_Core/src/AppController.cpp:1894) — bind to `ITrackerClient*`, not `GitHubClient*`:

- Comment-add → `ITrackerClient::AddIssueCommentPlain(cfg, issueKey, body, outError)` (virtual exists; Jira already implements; Plane gap → A.5).
- Label add / remove → `ITrackerClient::UpdateField(issueId, labelsField, currentLabels ∪ {label}, outError)` / `current \ {label}` (the diff is computed in a pure helper `ComputeLabelEditDiff` shared by all backends).
- Assignee set → `ITrackerClient::UpdateField(issueId, assigneeField, [user], outError)`.
- State transition → `ITrackerClient::UpdateField(issueId, statusField, [state], outError)`.

GitHub's `UpdateField` (filled in this plan) dispatches internally to the existing `CommentAdd` / `LabelAdd` / `LabelRemove` / `AssigneeSet` / `StateTransition` primitives — those primitives stay as the GitHub-side implementation detail; the audit-trail source `"github_client"` stays unchanged.

A.5 Fill `PlaneClient::AddIssueCommentPlain` (currently unimplemented — confirmed via grep). Jira already has it. Without this, triage on Plane silently drops clarification-mirror comments.

A.6 PR + check-run + GraphQL surface stays **GitHub-typed**. [`PrCheckRunWatcher`](../../Source_Core/include/PrCheckRunWatcher.h), [`PrCommentWatcher`](../../Source_Core/include/PrCommentWatcher.h), [`CiFailureClassifier`](../../Source_Core/include/CiFailureClassifier.h) keep their `GitHubClient::CheckRun` / `GitHubClient::CheckRunAnnotation` type dependencies. Rationale: Jira and Plane have no native PR or check-run concept. The CodeRabbit + CI react loops are GitHub-specific by construction. Cross-link: this carve-out is named in § Out of scope so a future "Bitbucket / GitLab review-bot integration" plan inherits the shape.

**B. GitHub-as-tracker fill-in** (lands second; depends on A.1, A.4).

B.1 De-gate the TU — drop `#if defined(SMATCHET_WITH_AGENTIC)` around the `GitHubClient` source list and `GitHubPat` config field. The build-time gate was the cheapest way to keep agentic code out of vanilla builds when the client landed; now that the same class serves both roles, the gate is the wrong axis. Runtime PAT-empty short-circuit means the code is reachable but zero-cost without configuration.

B.2 Implement the stubbed `ITrackerClient` virtuals on `GitHubClient` — `FetchIssues`, `FetchIssuesForKeys`, `ProbeReachability`, `BuildBrowseUrl`, `ExtractProjectFromQuery`, `ListProjects`, `FetchFieldCatalog` (static catalog, no API), `ResolveDisplayValue`, `UpdateIssueFields`, `UpdateField`, `BuildFieldPayload`, `BuildCreatePayload`, `CreateIssue`. The four write virtuals dispatch internally to the existing five write primitives plus one new shared `PatchIssue` helper for `title` / `body` / `milestone` (the three GitHub fields sharing the `PATCH /repos/{o}/{r}/issues/{n}` endpoint).

B.3 Wire the factory + config — extend `DefaultTrackerBackendFactory::Create` with a `"github"` branch; add `GitHubOwner`, `GitHubRepo`, `GitHubBaseUrl` to `TrackerConfig` (PAT already there); extend `SmatchetPreferencesUi` with the GitHub profile group. Schema bump deferred until the rollout verifies end-to-end (per [`AGENTS.md`](../../AGENTS.md) § Project rules § Schema-version bumps).

B.4 Single shared `GitHubClient` instance — promote `AppController::agenticGithubClient_` to `sharedGithubClient_`; both triage and tracker call sites resolve through `AppController::GetGithubClient()`. Factory's `Create("github")` returns a forwarding `unique_ptr` shell that delegates to the AppController-owned instance. Two-instance risk avoided.

Non-obvious trade-off: GitHub Issues has very few native fields (state, labels, assignees, milestone, title, body). Custom fields live on **Projects v2** behind GraphQL — separate effort, deferred. Phase B ships native fields only.

## Files to modify

Grouped by work-stream.

**Work-stream A — triage abstraction (lands first)**

1. [`Source_Core/include/ITrackerClient.h`](../../Source_Core/include/ITrackerClient.h:230) — add `FetchIssueBody` virtual (default sentinel); add `ListIssueKeysUpdatedSince` virtual.
2. [`Source_Core/include/AgenticTriageController.h`](../../Source_Core/include/AgenticTriageController.h:47) — rename `IGitHubReadClient` → `ITrackerTriageSource`; reshape `ListOpenIssuesForRepo(owner, repo, …)` → `ListIssueKeysUpdatedSince(query, …)` pass-through.
3. [`Source_Core/src/AgenticTriageController.cpp`](../../Source_Core/src/AgenticTriageController.cpp) — adapt to the renamed seam; no behavioural change.
4. [`Source_Core/src/AppController.cpp`](../../Source_Core/src/AppController.cpp:1766) — `GitHubReadAdapter` → `TrackerTriageAdapter` (templated or virtual-dispatched over the active `ITrackerClient*`); rebind triage write lambdas at [lines 1894-2036](../../Source_Core/src/AppController.cpp:1894) to call `ITrackerClient::AddIssueCommentPlain` + `UpdateField`.
5. **New file** [`Source_Core/include/TrackerTriageFieldIds.h`](../../Source_Core/include/) — per-backend field-id constants (`kJiraStatusFieldId = "status"`, `kPlaneStateFieldId = "state"`, `kGitHubStateFieldId = "state"`, plus labels + assignees). Triage write lambdas pick the right id from the active `ITrackerClient::GetTrackerType()`. One header, no .cpp.
6. **New file** [`Source_Core/src/PlaneIssueMutation.cpp`](../../Source_Core/src/PlaneIssueMutation.cpp) (extend existing) — implement `PlaneClient::AddIssueCommentPlain` to close the A.5 gap. Plane's wire is `POST /workspaces/{w}/projects/{p}/issues/{i}/comments/`.
7. [`Source_Core/src/JiraClient.cpp`](../../Source_Core/src/JiraClient.cpp) + [`JiraIssueSearch.cpp`](../../Source_Core/src/JiraIssueSearch.cpp) — implement `ListIssueKeysUpdatedSince` using JQL `updated >= <epoch-ms>`.
8. [`Source_Core/src/PlaneClient.cpp`](../../Source_Core/src/PlaneClient.cpp) + [`PlaneIssueSearch.cpp`](../../Source_Core/src/PlaneIssueSearch.cpp) — implement `ListIssueKeysUpdatedSince` using Plane's `updated_at__gte` filter.
9. **New pure helper** [`Source_Core/include/LabelEditDiffPure.h`](../../Source_Core/include/) + [`Source_Core/src/LabelEditDiffPure.cpp`](../../Source_Core/src/) — `ComputeLabelEditDiff(currentLabels, intendedLabels)` → `{toAdd, toRemove}`. Pure, no I/O, bucket-A testable. Used by every backend's label-edit path.

**Work-stream B — GitHub-as-tracker fill-in (lands second)**

10. [`Source_Core/include/GitHubClient.h`](../../Source_Core/include/GitHubClient.h:35) — drop "only `FetchIssueComments` is real this slice" comment; un-stub virtual declarations; add private `PatchIssue` helper.
11. [`Source_Core/src/GitHubClient.cpp`](../../Source_Core/src/GitHubClient.cpp:107) — replace stub bodies for `ProbeReachability` / `FetchIssues` / `FetchIssuesForKeys` / `UpdateIssueFields` / `UpdateField` / `BuildFieldPayload` / `ResolveDisplayValue`; add `BuildBrowseUrl`, `ExtractProjectFromQuery`, `ListProjects`, `FetchFieldCatalog`, `BuildCreatePayload`, `CreateIssue` overrides; add private `PatchIssue`. `UpdateField` routes by `field.id` → existing primitives + `PatchIssue`.
12. **New file** [`Source_Core/src/GitHubIssueSearch.cpp`](../../Source_Core/src/) — paginated `FetchIssues` + `FetchIssuesForKeys` + `ListIssueKeysUpdatedSince` impl. Mirrors `JiraIssueSearch.cpp` / `PlaneIssueSearch.cpp` split.
13. **New file** [`Source_Core/src/GitHubFieldCatalog.cpp`](../../Source_Core/src/) — static catalog (6 fields) + `ResolveDisplayValue`. Mirrors `PlaneFieldCatalog.cpp`.
14. [`Source_Core/include/GitHubClientHelpers.h`](../../Source_Core/include/GitHubClientHelpers.h) + `.cpp` — add `BuildIssuePatchSuffix`, `BuildIssuesListSuffix`.
15. [`Source_Core/src/DefaultTrackerBackendFactory.cpp`](../../Source_Core/src/DefaultTrackerBackendFactory.cpp:7) — `"github"` branch; forwarding shell delegating to `AppController::GetGithubClient()`.
16. [`Source_Core/include/ConfigManager.h`](../../Source_Core/include/ConfigManager.h:222) — un-gate `GitHubPat` (drop `#if SMATCHET_WITH_AGENTIC`); add `GitHubBaseUrl`, `GitHubOwner`, `GitHubRepo`.
17. [`Source_Core/src/ConfigManager.cpp`](../../Source_Core/src/ConfigManager.cpp) — Load / Save the three new fields; legacy-config migration.
18. [`Source_Core/src/SmatchetPreferencesUi.cpp`](../../Source_Core/src/SmatchetPreferencesUi.cpp) — GitHub profile group under Tracker section.
19. [`Source_Core/include/AppController.h`](../../Source_Core/include/AppController.h:1276) — rename `EnsureAgenticGithubClient` → `GetGithubClient`; rename member.
20. [`CMakeLists.txt`](../../CMakeLists.txt) — drop `SMATCHET_WITH_AGENTIC` from `GitHubClient*.cpp` source-list condition; the agentic-flow-only test TUs (`GitHubClient_PrSurface.test.cpp`, `GitHubClient_GraphQL.test.cpp`) stay gated separately.

**Tests (Bucket A — pure-logic ctest)**

21. [`tests/Source_Core/LabelEditDiffPure.test.cpp`](../../tests/Source_Core/) — new. Pure-set-diff exhaustive cases.
22. [`tests/Source_Core/TrackerTriageAdapter.test.cpp`](../../tests/Source_Core/) — new. Backend-agnostic triage: mock `ITrackerClient` returns canned issue/comments; assert adapter dispatches correctly across "github" / "jira" / "plane" types.
23. [`tests/Source_Core/GitHubClient_FieldCatalog.test.cpp`](../../tests/Source_Core/) — new. Static catalog shape (6 fields, allowed values).
24. [`tests/Source_Core/GitHubClient_UpdateField.test.cpp`](../../tests/Source_Core/) — new. Field-id router dispatch: state → `StateTransition`, labels diff → `LabelAdd`/`LabelRemove`, assignee → `AssigneeSet`, title/body/milestone → `PatchIssue`. Mocked HTTP per existing `GitHubClient_GraphQL.test.cpp` pattern. Critical assertion: no field-id path makes a raw HTTP call outside the primitive set.
25. [`tests/Source_Core/GitHubClientHelpers.test.cpp`](../../tests/Source_Core/GitHubClientHelpers.test.cpp) — extend with `BuildIssuePatchSuffix`, `BuildIssuesListSuffix`, `ExtractProjectFromQuery`.
26. [`tests/Source_Core/GitHubFieldCatalog.test.cpp`](../../tests/Source_Core/) — new. `ResolveDisplayValue` for assignees + labels.
27. [`tests/Source_Core/PlaneClient_AddComment.test.cpp`](../../tests/Source_Core/) — new. Plane comment-write happy path + redaction.
28. [`tests/Source_Core/JiraClient_ListUpdatedSince.test.cpp`](../../tests/Source_Core/) — new. JQL `updated >= …` shape.

## Existing utilities reused

Anti-duplication contract lives here.

**Triage refactor (A)**:

- `JiraClient::AddIssueCommentPlain` — [`JiraIssueMutation.cpp:298`](../../Source_Core/src/JiraIssueMutation.cpp:298) — already exists; triage write-comment path uses it unchanged.
- `ITrackerClient::AddIssueCommentPlain` + `UpdateField` virtuals — [`ITrackerClient.h:215`](../../Source_Core/include/ITrackerClient.h:215) + line 136 — present, no API addition needed for writes.
- `AgenticTriageController` core logic — [`AgenticTriageController.cpp`](../../Source_Core/src/AgenticTriageController.cpp) — sequence stays (`FetchIssueBody` → `FetchIssueComments` → `RequestProposals` → `Insert`). Only the seam-type changes.
- `AgentTriageScenarioStep`'s `ScenarioMockGitHub` — [`Commands/Scenarios/AgentTriageScenarioStep.cpp`](../../Source_Core/src/Commands/Scenarios/AgentTriageScenarioStep.cpp) — renamed to `ScenarioMockTrackerTriageSource`; same canned-issue logic.

**GitHub fill-in (B)**:

- `GitHubClient::CommentAdd` + `LabelAdd` + `LabelRemove` + `AssigneeSet` + `StateTransition` — [GitHubClient.h:134-168](../../Source_Core/include/GitHubClient.h:134) — `UpdateField` is a router; no new HTTP path.
- `GitHubClient::ListOpenIssuesForRepo` `since=` cursor + per_page cap — promoted to `GitHubClientHelpers::BuildIssuesListSuffix` so list + tracker-sync share parameter encoding.
- `GitHubClient::FetchIssueBody` — moved up to `ITrackerClient` virtual; no signature change.
- `MakeGitHubAuthHeaders` + `ComposeHttpErrorString` + `RedactForLog` — [GitHubClient.cpp:38-69](../../Source_Core/src/GitHubClient.cpp:38) — every new HTTP call uses these. Inline-bearer pattern stays per [`agentic-flow-implementation.md`](agentic-flow-implementation.md) § Decisions locked #2.
- `GitHubClientHelpers::ParseGitHubIssueKey` + `BuildIssue*Suffix` family.
- `BackendAuditTrail::AppendBegin`/`AppendResult` with `source="github_client"` — triage-flow audit-consumers (filter on `source`) keep working unchanged.
- `CachedTicket` + `LocalCacheManager` — shared shape across all backends.
- `TrackerFieldCatalogResult` + `TrackerField` — [`TrackerFieldSchema.h`](../../Source_Core/include/TrackerFieldSchema.h).
- `AppController::EnsureAgenticGithubClient` lazy + `std::call_once` ownership — renamed + reused; no second instance.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: tracker fetch + field edits are off-UI by existing `ITrackerClient` contract. Triage refactor changes the dispatch type at lambda-bind time, not the call-thread. Steady-state UI work unaffected.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: every new HTTP path preserves existing 5s connect / 15s overall timeout (`kGitHubConnectTimeoutMs` / `kGitHubOverallTimeoutMs`); Jira/Plane existing timeouts unchanged. All triage + tracker-sync call sites are worker-thread (`TicketSyncService`, field-edit pipeline, scheduled-poll worker, AgenticTriageController). No new UI-thread entry points.
- **Pillar 3 (never crash)**: every new method follows existing error-handling discipline — PAT-presence check first, parse-fail returns `false` + `outError`, no raw `new` / `delete`, JSON wrapped in `try` / `catch`. Sanitizer build via `ninja-test-msys2` per project rules. The interface rename is mechanical (`IGitHubReadClient` → `ITrackerTriageSource`); type errors at compile time catch any miss.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no new UI widgets beyond the Preferences profile group, which reuses existing `SmatchetPreferencesUi` conventions. N/A as regression risk.

## Perf-review-system gates

Per [`docs/design/pillar-1-2-perf-review-system.md`](pillar-1-2-perf-review-system.md). Diff touches `Source_Core/` — gates apply.

1. **PR-fast CI** — primary scenario: `tracker_sync` (added GitHub backend path); secondary: `agentic_triage_poll` (refactored seam). Verify both are in `scripts/dev/perf-pr-fast-set.json`; if not, declare in PR body which scenario covers the change.
2. **Pillar 2 static scanner** — no new sync-I/O reachable from `ImGui::*`. All HTTP / parse work stays on worker threads. `/* PILLAR2_WORKER_ONLY */` annotation not required.
3. **Dispatcher drain** — no `MainThreadDispatcher::Drain()` touch.
4. **Visible-cue bucket-E harness** — no new sync stalls > 100 ms.
5. **Marker inventory** — no new `SMATCHET_UI_PERF_SCOPE` markers planned. If implementer adds any for tracker-fetch hot paths, regen `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push local check**: `bash scripts/dev/perf-run.sh tracker_sync` + `bash scripts/dev/perf-run.sh agentic_triage_poll` + `perf-compare.py` against any existing baseline. `MISSING_BASELINE` acceptable on first run; CI auto-bootstraps.

**Override**: `perf-out-of-band` PR label per [`AGENTS.md`](../../AGENTS.md) § Merge gates — not expected.

## Risks / non-goals

**Risks**:

- **Triage regression on GitHub** — the highest risk. The refactor reroutes every triage write from `GitHubClient::*` to `ITrackerClient::*`. A subtle audit-trail or error-path divergence would silently break the agentic flow. Mitigation: (a) the existing `GitHubClient::CommentAdd`/`LabelAdd`/etc. methods stay as the GitHub-side implementation — `UpdateField` is a router, not a replacement; (b) audit-trail source string `"github_client"` stays unchanged (verified explicitly in test 24); (c) full `agentic_triage_poll` + `coderabbit_react` + `ci_react` Bucket E scenarios run as merge gate.
- **Plane `AddIssueCommentPlain` gap** — Plane today returns the "not supported" sentinel for comment writes. Triage on Plane silently dropped any clarification-mirror today; the refactor surfaces this. Mitigation: A.5 fills the gap; without it, the refactor PR is incomplete.
- **Jira `ListIssueKeysUpdatedSince` JQL shape divergence** — Jira's `updated >= <ms>` expects millisecond epoch; GitHub's `since=` expects ISO 8601. Mitigation: per-backend implementation in `JiraIssueSearch.cpp` / `PlaneIssueSearch.cpp` / `GitHubIssueSearch.cpp`; the seam returns canonical issue-keys + uses backend-native time format internally.
- **Two `GitHubClient` instances** — naive wire-in creates two PAT-holding clients per process. Mitigation per B.4 — `AppController::GetGithubClient()` is the single owner; factory returns a forwarding shell.
- **`SMATCHET_WITH_AGENTIC` un-gating** — drops triage-only code into vanilla builds (~150 LOC of additional cpr call sites). Accepted; the runtime PAT-empty short-circuit means dead-code at zero cost.
- **GitHub PATs vs OAuth** — current design keeps PAT-only. `TrackerConfig::GitHubBaseUrl` lets users point at GitHub Enterprise. OAuth is a follow-up plan.
- **GitHub Projects v2 custom fields** — phase B ships native fields only. Static catalog leaves room for a GraphQL-fetched Projects-v2 catalog merge later. § Out of scope flags the follow-up.
- **GitHub rate limits** — 5000/hr PAT-authed; `FetchIssues` paginating at 100/page comfortable. 403 surfaces via existing `ComposeHttpErrorString`.

**Non-goals**:

- **PR + check-run + CodeRabbit GraphQL surface** — stays GitHub-only. Jira and Plane have no native PR or check-run concept; the CodeRabbit + CI react loops are GitHub-specific by construction. Future "Bitbucket / GitLab review-bot integration" plans inherit the same carve-out shape.
- **Projects v2 custom fields via GraphQL** — separate plan.
- **GitHub Issues comment writes from the tracker grid (rich-text UI)** — `AddIssueCommentPlain` virtual maps to `CommentAdd`, but the rich-text editor surface stays Jira/Plane-only this phase.
- **PR-as-issue tracking** — tracker stays issues-only; PRs continue to live in the agentic-flow PR-watcher surface, not the grid.
- **GitHub Enterprise OAuth / SSO** — PAT only this phase.
- **Migration tooling** — no Jira-to-GitHub or Plane-to-GitHub issue migration.

## Verification

Per [`AGENTS.md`](../../AGENTS.md) § Verification automation — zero manual steps.

- **Bucket A (pure-logic ctest, `test-rig`)**:
  - `LabelEditDiffPure.test.cpp` — `ComputeLabelEditDiff` exhaustive set-diff cases.
  - `TrackerTriageAdapter.test.cpp` — mock `ITrackerClient` (per `GetTrackerType()` = "github"/"jira"/"plane") returns canned issue + comments; assert triage adapter dispatches `FetchIssueBody` → `FetchIssueComments` → `RequestProposals` correctly across all three.
  - `GitHubClient_FieldCatalog.test.cpp` — static catalog shape.
  - `GitHubClient_UpdateField.test.cpp` — router dispatch; **no field-id path makes a raw HTTP call outside the primitive set**.
  - `GitHubFieldCatalog.test.cpp` — `ResolveDisplayValue`.
  - `GitHubClientHelpers.test.cpp` (extend) — new helper round-trips.
  - `PlaneClient_AddComment.test.cpp` — closes A.5 gap with mock HTTP.
  - `JiraClient_ListUpdatedSince.test.cpp` — JQL shape.
- **Bucket E (ImGui Test Engine)**: extend tracker-switching scenario (if present at `tests/ui/`) to flip Jira / Plane / GitHub and assert grid rows populate. Follow-up `docs/backlog/agent-self-improvement/test.md` entry if no such scenario exists.
- **Bash-driver scenarios**:
  - `scripts/dev/test-github-tracker.sh` (new) — spawn standalone, run `tracker.set github`, `tracker.sync`, assert ≥ 1 row in SQLite cache. Skip when test PAT unset.
  - `scripts/dev/test-triage-jira.sh` (new) — same shape for Jira-as-triage-source; skip when Jira creds unset.
- **Build gate**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — dual-target; must pass with `SMATCHET_WITH_AGENTIC=OFF` (no-agentic build still compiles `GitHubClient`).
- **Sanitizer gate**: `cmake --build --preset ninja-test-msys2` — ASan/UBSan clean.
- **Perf gate**: `bash scripts/dev/perf-run.sh tracker_sync` + `agentic_triage_poll` — see § Perf-review-system gates.
- **Manual residue**: live-PAT smoke (one per backend) at first end-to-end run. Same gate Jira/Plane have today. Test PATs in CI = security non-starter.

## Out of scope (flagged, not designed)

- **Projects v2 custom fields** — follow-up `docs/design/github-projects-v2-fields.md`.
- **Bitbucket / GitLab review-bot integration** — would extend the GitHub-only PR/check-run carve-out via new typed clients per provider; not generalized at the `ITrackerClient` layer (different review-bot semantics).
- **GitHub Apps + OAuth** — follow-up; PATs cover the immediate need.
- **Repo-multi-select on a single tracker profile** — phase B is one `owner/repo` per profile.
- **GitHub-side audit-trail consumer** — existing `BackendAuditTrail` captures all writes; no new consumer needed.
- **Rich-text comment editor for GitHub on the grid** — Jira/Plane-only this phase.

## Implementation log
*(populated post-ship per [`AGENTS.md`](../../AGENTS.md) § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
