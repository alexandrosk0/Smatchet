# Plan — GitHub as a third tracker backend (+ triage tracker-agnostic refactor)

> **Slug**: `github-tracker-backend`
>
> **Mandatory rules cross-link**: see [`AGENTS.md`](../../AGENTS.md) § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.
>
> **Stress-test pass**: 2026-05-21 grill-with-docs session locked the eight design decisions in § Decisions locked below. ADRs: [`0003-github-as-itrackerclient`](../adr/0003-github-as-itrackerclient.md), [`0007-audit-trail-actor-column`](../adr/0007-audit-trail-actor-column.md). Glossary additions: [`CONTEXT.md`](../CONTEXT.md) § Source tracker, § Code host, § HandoffClarificationPostToSourceTracker, § `UpdateField` semantics — set-replace, § Audit-trail actor.

## Context

Smatchet today ships two grid-backing tracker backends — Jira and Plane — wired via `ITrackerClient` and `DefaultTrackerBackendFactory`. A third surface, GitHub, already exists in-tree as [`GitHubClient`](../../Source_Core/include/GitHubClient.h) but is currently **triage-only**: it powers the agentic-flow pipeline (CodeRabbit + CI react loop, `handoff-implementer`, `pr-iterator`, the T7 scheduled-poll worker) and stubs out every grid-relevant `ITrackerClient` virtual with the documented "not supported on GitHub backend yet" sentinel.

Triage's own seam — [`IGitHubReadClient`](../../Source_Core/include/AgenticTriageController.h:47) — is **GitHub-shaped**: `ListOpenIssuesForRepo(owner, repo, ...)` takes a GitHub URL fragment. AppController binds triage write lambdas (`CommentAdd` / `LabelAdd` / `AssigneeSet` / `StateTransition`) **directly** to `GitHubClient::*` methods. Two hardcoded gates in [`AgenticTriageController.cpp`](../../Source_Core/src/AgenticTriageController.cpp) cement the GitHub-only assumption: line 79 (`p.sourceTracker = "github"` hardcode despite the parameter being available) and line 104 (`if (sourceTracker != "github") return false`). The schema below the controller is already multi-tracker-ready — `AgentProposal.sourceTracker` ∈ {`github`, `jira`, `plane`} column exists; `agent_poll_cursor(source_tracker, repo_key)` composite-PK exists. **The block is two code-level gates, not a data-model change.**

User ask, two parts:

1. Let users pick **GitHub** as the active tracker so issues populate the grid, sync into the SQLite cache, and support field-edit + create-issue flows the same way Jira/Plane do.
2. **Refactor triage to use `ITrackerClient`** so the agentic-flow's discovery + comment + label + state-transition + clarification paths work against Jira and Plane sources too.

Cross-link: [`docs/design/agentic-flow-implementation.md`](agentic-flow-implementation.md); [`docs/design/agentic-triage-flow.md`](agentic-triage-flow.md); [`docs/design/coderabbit-react-loop.md`](coderabbit-react-loop.md); [`docs/design/agentic-coding-handoff.md`](agentic-coding-handoff.md) (PR-side flows that stay code-host-coupled).

## Decisions locked

Locked by the 2026-05-21 grill-with-docs session. Diverging from any of these = needs a plan revision, not an implementation choice.

1. **Source tracker ≠ code host** — issue lives on any tracker; PR + CodeRabbit + check-runs live on GitHub (today). Asymmetry named in CONTEXT.md § Source tracker / § Code host. `HandoffClarificationPostToGithub` → renamed `HandoffClarificationPostToSourceTracker`.
2. **`UpdateField` semantics = set-replace at the virtual** — `values` is the intended full set after edit. Jira / Plane native; GitHub impl pre-fetches the current set and diffs internally. CONTEXT.md § `UpdateField` semantics — set-replace.
3. **Audit-trail `actor` column added** — discriminates trigger (`user` / `triage` / `ci_react` / `coderabbit_react` / `lua` / `mcp`) from `source` (backend client) and `action` (verb). ADR [`0007`](../adr/0007-audit-trail-actor-column.md). Schema bump bundled with this plan (`agent_audit_trail` migration).
4. **`ITrackerTriageSource` adapter dropped** — `AgenticTriageController` takes `ITrackerClient*` directly. `IGitHubReadClient` is deleted, not renamed. Test mocks subclass `ITrackerClient` and override the 3 methods the controller calls; the base-class defaults cover the other 27 virtuals.
5. **Single tracker config — poll source = active grid tracker** — `AgenticPollSource` config field retired (silently ignored on Load for legacy configs). Worker reads `TrackerType` to pick the backend; dispatches `ITrackerClient::ListIssueKeysUpdatedSince(query, sinceUnixSec)`. `AgenticPollQuery` stays as the per-tracker query.
6. **Single bundled PR** — work-streams A + B land together. No intermediate state where A's triage rebind hits B's still-stubbed GitHub `UpdateField`.
7. **Verification = Bucket A + new Bucket B scenario** — pure-logic ctests cover routing + diff; new `multi_tracker_triage_smoke` scenario runs the triage controller against three canned `ITrackerClient` mocks. No bucket E this PR; deferred backlog entry for Preferences-UI tracker-switch coverage.
8. **GitHub `FetchIssueBody` promotes to `ITrackerClient` virtual; new `ListIssueKeysUpdatedSince` virtual added** — Jira + Plane implement both; GitHub already has FetchIssueBody concretely. JQL `updated >= <epoch-ms>` for Jira; `updated_at__gte` for Plane; `since=<iso8601>` for GitHub.

## Approach

Two parallel work-streams ship in **one squashed PR** (decision 6). Code-level the changes can land in any topological order; the PR ships them together so triage never sees a moment where `UpdateField` is rebound onto a stubbed GitHub impl.

**A. Triage tracker-agnostic refactor**

A.1 Promote `FetchIssueBody` to `ITrackerClient` as a new virtual (default sentinel). Jira + Plane fill it in; GitHubClient already has it.

A.2 Add `ITrackerClient::ListIssueKeysUpdatedSince(const TrackerConfig& cfg, const std::string& query, std::int64_t sinceUnixSec, std::vector<std::string>& outKeys, std::string& outError)` virtual (default sentinel). Backend-specific query shape: `owner/repo` for GitHub, JQL for Jira, project-UUID-filter for Plane. Returns canonical issue keys (`owner/repo#N`, `PROJ-42`, `<plane-uuid>`).

A.3 **Delete `IGitHubReadClient`** (decision 4). `AgenticTriageController` ctor takes `ITrackerClient*` directly. `GitHubReadAdapter` deleted. Test mocks subclass `ITrackerClient`.

A.4 Lift the two hardcoded GitHub-only gates in [`AgenticTriageController.cpp`](../../Source_Core/src/AgenticTriageController.cpp):
- Line 79: `p.sourceTracker = "github"` → `p.sourceTracker = sourceTracker` (use the param).
- Line 104: `if (sourceTracker != "github") return false` → accept all backends; validate against `ITrackerClient::GetTrackerType()` match instead.

A.5 Refactor triage write lambdas in [`AppController.cpp`](../../Source_Core/src/AppController.cpp:1894) — bind to `ITrackerClient*`, not `GitHubClient*`:
- Comment-add → `ITrackerClient::AddIssueCommentPlain(cfg, issueKey, body, outError)`.
- Label set → `ITrackerClient::UpdateField(issueId, labelsField, intendedFullSet, outError)` (set-replace per decision 2; GitHub impl diffs internally).
- Assignee set → `ITrackerClient::UpdateField(issueId, assigneeField, [user], outError)`.
- State transition → `ITrackerClient::UpdateField(issueId, statusField, [state], outError)`.

A.6 Per-backend field-id constants in new header [`TrackerTriageFieldIds.h`](../../Source_Core/include/) — Jira `"status"` / `"labels"` / `"assignee"`; Plane `"state"` / `"labels"` / `"assignees"`; GitHub `"state"` / `"labels"` / `"assignees"`. Triage write lambdas pick by `ITrackerClient::GetTrackerType()`.

A.7 Collapse `AgenticPollSource` (decision 5) — remove from config; scheduled-poll worker reads `TrackerType` to pick backend, dispatches `ITrackerClient::ListIssueKeysUpdatedSince(query, sinceUnixSec)` against it. Legacy `AgenticPollSource` JSON ignored on Load.

A.8 Fill `PlaneClient::AddIssueCommentPlain` (currently the unsupported sentinel). Without this, Plane triage silently drops clarification-mirror comments. Wire shape: `POST /workspaces/{w}/projects/{p}/issues/{i}/comments/`.

A.9 Implement `JiraClient::ListIssueKeysUpdatedSince` (JQL `updated >= <epoch-ms>`) and `PlaneClient::ListIssueKeysUpdatedSince` (Plane `updated_at__gte` filter). GitHub's impl extracts from existing `ListOpenIssuesForRepo` plumbing.

A.10 **PR + check-run + GraphQL surface stays GitHub-only** (decision 1). [`PrCheckRunWatcher`](../../Source_Core/include/PrCheckRunWatcher.h), [`PrCommentWatcher`](../../Source_Core/include/PrCommentWatcher.h), [`CiFailureClassifier`](../../Source_Core/include/CiFailureClassifier.h) keep their `GitHubClient::CheckRun` / `GitHubClient::CheckRunAnnotation` typed dependencies. Jira and Plane have no native PR or check-run concept; the CodeRabbit + CI react loops are code-host-coupled by construction.

A.11 Rename `HandoffClarificationPostToGithub` → `HandoffClarificationPostToSourceTracker` (decision 1). Behavior unchanged for GitHub source; Jira / Plane source paths now route through `ITrackerClient::AddIssueCommentPlain`.

A.12 Audit-trail schema bump (decision 3 + ADR 0007) — `agent_audit_trail.actor` column with `DEFAULT 'user'`. `BackendAuditTrail::AppendBegin` / `AppendResult` gain an `actor` defaulted parameter. Triage / ci-react / coderabbit-react / lua / mcp call sites pass their explicit actor; user-facing UI surfaces accept the default.

**B. GitHub-as-tracker fill-in**

B.1 De-gate the TU — drop `#if defined(SMATCHET_WITH_AGENTIC)` around the `GitHubClient` source list and `GitHubPat` config field. Runtime PAT-empty short-circuit means dead-code at zero cost.

B.2 Implement the stubbed `ITrackerClient` virtuals on `GitHubClient` — `FetchIssues`, `FetchIssuesForKeys`, `ProbeReachability`, `BuildBrowseUrl`, `ExtractProjectFromQuery`, `ListProjects`, `FetchFieldCatalog` (static catalog, no API), `ResolveDisplayValue`, `UpdateIssueFields`, `UpdateField`, `BuildFieldPayload`, `BuildCreatePayload`, `CreateIssue`.

The write virtuals dispatch internally to existing five primitives plus one new shared `PatchIssue` helper for `title` / `body` / `milestone`. `UpdateField` for set fields (labels, assignees) does the internal pre-fetch + diff per decision 2:

```
UpdateField(issueId, "labels", desiredSet):
    parse issueId via ParseGitHubIssueKey
    GET /repos/{o}/{r}/issues/{n} → currentLabels
    {toAdd, toRemove} = ComputeLabelEditDiff(currentLabels, desiredSet)
    for each in toAdd: LabelAdd(...)
    for each in toRemove: LabelRemove(...)
    audit-trail one "field_update_labels" row spanning the batch (begin/result)
```

B.3 Wire the factory + config — extend `DefaultTrackerBackendFactory::Create` with a `"github"` branch; add `GitHubBaseUrl`, `GitHubOwner`, `GitHubRepo` to `TrackerConfig` (PAT already there); extend `SmatchetPreferencesUi` with the GitHub profile group.

B.4 Single shared `GitHubClient` instance — promote `AppController::agenticGithubClient_` to `sharedGithubClient_`; both triage and tracker call sites resolve through `AppController::GetGithubClient()`. Factory's `Create("github")` returns a forwarding `unique_ptr` shell that delegates to the AppController-owned instance.

Non-obvious trade-off: GitHub Issues has few native fields. Projects v2 custom fields (GraphQL) are a separate effort — § Out of scope flags the follow-up.

## Files to modify

Grouped by work-stream; one squashed PR.

**Work-stream A — triage abstraction + audit-trail bump**

1. [`Source_Core/include/ITrackerClient.h`](../../Source_Core/include/ITrackerClient.h:230) — add `FetchIssueBody` + `ListIssueKeysUpdatedSince` virtuals (default sentinel).
2. [`Source_Core/include/AgenticTriageController.h`](../../Source_Core/include/AgenticTriageController.h:47) — **delete `IGitHubReadClient`**; ctor takes `ITrackerClient*` directly.
3. [`Source_Core/src/AgenticTriageController.cpp`](../../Source_Core/src/AgenticTriageController.cpp:79) + [`:104`](../../Source_Core/src/AgenticTriageController.cpp:104) — lift two hardcoded GitHub gates; rewire to `ITrackerClient*`.
4. [`Source_Core/src/AppController.cpp`](../../Source_Core/src/AppController.cpp:1766) — delete `GitHubReadAdapter` class (lines 1766-1828); rebind triage write lambdas at [lines 1894-2036](../../Source_Core/src/AppController.cpp:1894) to call `ITrackerClient::*` virtuals; scheduled-poll worker reads `TrackerType` instead of `AgenticPollSource`.
5. **New file** [`Source_Core/include/TrackerTriageFieldIds.h`](../../Source_Core/include/) — per-backend field-id constants for labels / assignees / status. One header, no .cpp.
6. [`Source_Core/src/PlaneClient.cpp`](../../Source_Core/src/PlaneClient.cpp) + [`PlaneIssueMutation.cpp`](../../Source_Core/src/PlaneIssueMutation.cpp) — implement `AddIssueCommentPlain`.
7. [`Source_Core/src/JiraClient.cpp`](../../Source_Core/src/JiraClient.cpp) + [`JiraIssueSearch.cpp`](../../Source_Core/src/JiraIssueSearch.cpp) — implement `ListIssueKeysUpdatedSince` (JQL `updated >= <ms>`).
8. [`Source_Core/src/PlaneClient.cpp`](../../Source_Core/src/PlaneClient.cpp) + [`PlaneIssueSearch.cpp`](../../Source_Core/src/PlaneIssueSearch.cpp) — implement `ListIssueKeysUpdatedSince` (`updated_at__gte`).
9. **New pure helper** [`Source_Core/include/LabelEditDiffPure.h`](../../Source_Core/include/) + [`Source_Core/src/LabelEditDiffPure.cpp`](../../Source_Core/src/) — `ComputeLabelEditDiff(currentLabels, intendedLabels) → {toAdd, toRemove}`. Pure, no I/O, bucket-A testable. Used by GitHub `UpdateField` internal diff.
10. [`Source_Core/include/BackendAuditTrail.h`](../../Source_Core/include/BackendAuditTrail.h) + [`.cpp`](../../Source_Core/src/BackendAuditTrail.cpp) — add `actor` defaulted parameter; persist new column.
11. [`Source_Core/src/AgentProposalStore.cpp`](../../Source_Core/src/AgentProposalStore.cpp) — schema migration step adding `agent_audit_trail.actor TEXT DEFAULT 'user'`. Bump schema-version.
12. [`Source_Core/include/ConfigManager.h`](../../Source_Core/include/ConfigManager.h:222) + [`.cpp`](../../Source_Core/src/ConfigManager.cpp) — un-gate `GitHubPat`; **remove** `AgenticPollSource` from struct; add `GitHubBaseUrl`, `GitHubOwner`, `GitHubRepo`; rename `HandoffClarificationPostToGithub` → `HandoffClarificationPostToSourceTracker`. Legacy `AgenticPollSource` ignored on Load.
13. All triage / ci-react / coderabbit-react / lua / mcp audit call sites (~30 sites across `*ReactController.cpp`, `JiraIssueMutation.cpp`, `PlaneIssueMutation.cpp`, `GitHubClient.cpp` write methods, `LuaConsole/`, `Mcp/`) — pass explicit `actor`.

**Work-stream B — GitHub-as-tracker fill-in**

14. [`Source_Core/include/GitHubClient.h`](../../Source_Core/include/GitHubClient.h:35) — un-stub virtual declarations; add private `PatchIssue` helper.
15. [`Source_Core/src/GitHubClient.cpp`](../../Source_Core/src/GitHubClient.cpp:107) — replace stub bodies; add `BuildBrowseUrl`, `ExtractProjectFromQuery`, `ListProjects`, `FetchFieldCatalog`, `BuildCreatePayload`, `CreateIssue` overrides; add private `PatchIssue`. `UpdateField` routes by `field.id` → existing primitives + diff-via-`LabelEditDiffPure` for set fields.
16. **New file** [`Source_Core/src/GitHubIssueSearch.cpp`](../../Source_Core/src/) — paginated `FetchIssues` + `FetchIssuesForKeys` + `ListIssueKeysUpdatedSince`. Mirrors `JiraIssueSearch.cpp` split.
17. **New file** [`Source_Core/src/GitHubFieldCatalog.cpp`](../../Source_Core/src/) — static catalog (6 fields) + `ResolveDisplayValue`. Mirrors `PlaneFieldCatalog.cpp`.
18. [`Source_Core/include/GitHubClientHelpers.h`](../../Source_Core/include/GitHubClientHelpers.h) + `.cpp` — add `BuildIssuePatchSuffix`, `BuildIssuesListSuffix`.
19. [`Source_Core/src/DefaultTrackerBackendFactory.cpp`](../../Source_Core/src/DefaultTrackerBackendFactory.cpp:7) — `"github"` branch; forwarding-shell delegating to `AppController::GetGithubClient()`.
20. [`Source_Core/src/SmatchetPreferencesUi.cpp`](../../Source_Core/src/SmatchetPreferencesUi.cpp) — GitHub profile group; remove `AgenticPollSource` widget; rename clarification toggle.
21. [`Source_Core/include/AppController.h`](../../Source_Core/include/AppController.h:1276) — rename `EnsureAgenticGithubClient` → `GetGithubClient`; rename member.
22. [`CMakeLists.txt`](../../CMakeLists.txt) — drop `SMATCHET_WITH_AGENTIC` from `GitHubClient*.cpp` source-list; the agentic-flow-only test TUs (`GitHubClient_PrSurface.test.cpp`, `GitHubClient_GraphQL.test.cpp`) stay gated separately.

**Tests**

23. [`tests/Source_Core/LabelEditDiffPure.test.cpp`](../../tests/Source_Core/) — pure-set-diff exhaustive cases.
24. [`tests/Source_Core/TrackerTriageDispatch.test.cpp`](../../tests/Source_Core/) — mock `ITrackerClient` returning canned issue/comments across "github" / "jira" / "plane" types; assert `AgenticTriageController` produces correct `sourceTracker` field on the proposal row.
25. [`tests/Source_Core/GitHubClient_FieldCatalog.test.cpp`](../../tests/Source_Core/) — static catalog shape.
26. [`tests/Source_Core/GitHubClient_UpdateField.test.cpp`](../../tests/Source_Core/) — router dispatch + internal labels-diff; **no field-id path makes a raw HTTP call outside the primitive set**; the GET-current-labels pre-fetch is asserted on every set-field update.
27. [`tests/Source_Core/GitHubFieldCatalog.test.cpp`](../../tests/Source_Core/) — `ResolveDisplayValue` for assignees + labels.
28. [`tests/Source_Core/GitHubClientHelpers.test.cpp`](../../tests/Source_Core/GitHubClientHelpers.test.cpp) — extend with `BuildIssuePatchSuffix`, `BuildIssuesListSuffix`, `ExtractProjectFromQuery`.
29. [`tests/Source_Core/PlaneClient_AddComment.test.cpp`](../../tests/Source_Core/) — Plane comment-write happy path + redaction.
30. [`tests/Source_Core/JiraClient_ListUpdatedSince.test.cpp`](../../tests/Source_Core/) — JQL shape.
31. [`tests/Source_Core/AuditTrail_Actor.test.cpp`](../../tests/Source_Core/) — `actor` column default + explicit-pass round-trips.
32. **New bucket-B scenario** [`Source_Core/src/Commands/Scenarios/MultiTrackerTriageSmoke.cpp`](../../Source_Core/src/Commands/Scenarios/) — triage controller runs against canned `ITrackerClient` mocks for "github" / "jira" / "plane" in sequence; asserts on `agent_proposals` row count + `sourceTracker` field per source.

## Existing utilities reused

Anti-duplication contract.

**Triage refactor (A)**:

- `JiraClient::AddIssueCommentPlain` — [`JiraIssueMutation.cpp:298`](../../Source_Core/src/JiraIssueMutation.cpp:298) — already exists.
- `ITrackerClient::AddIssueCommentPlain` + `UpdateField` virtuals — [`ITrackerClient.h:215`](../../Source_Core/include/ITrackerClient.h:215) + line 136 — present; no API addition for writes.
- `AgenticTriageController` core sequence — `FetchIssueBody` → `FetchIssueComments` → `RequestProposals` → `Insert` — unchanged.
- `AgentProposal.sourceTracker` column + `agent_poll_cursor(source_tracker, repo_key)` composite PK — already exist; reused, no schema change for the multi-tracker data model.
- `BackendAuditTrail::AppendBegin/AppendResult` — extended with defaulted `actor`; no existing call site needs change for grid surfaces.

**GitHub fill-in (B)**:

- `GitHubClient::CommentAdd` + `LabelAdd` + `LabelRemove` + `AssigneeSet` + `StateTransition` — [GitHubClient.h:134-168](../../Source_Core/include/GitHubClient.h:134) — `UpdateField` is a router; no new HTTP path.
- `GitHubClient::ListOpenIssuesForRepo` `since=` cursor + per_page cap — promoted to `GitHubClientHelpers::BuildIssuesListSuffix` so list + tracker-sync share parameter encoding.
- `GitHubClient::FetchIssueBody` — moved up to `ITrackerClient` virtual; no signature change.
- `MakeGitHubAuthHeaders` + `ComposeHttpErrorString` + `RedactForLog` — [GitHubClient.cpp:38-69](../../Source_Core/src/GitHubClient.cpp:38) — every new HTTP call uses these.
- `GitHubClientHelpers::ParseGitHubIssueKey` + `BuildIssue*Suffix` family.
- `CachedTicket` + `LocalCacheManager` — shared shape across all backends.
- `TrackerFieldCatalogResult` + `TrackerField` — [`TrackerFieldSchema.h`](../../Source_Core/include/TrackerFieldSchema.h).
- `AppController::EnsureAgenticGithubClient` lazy + `std::call_once` ownership — renamed + reused; no second instance.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: tracker fetch + field edits stay off-UI. Triage refactor changes dispatch type at lambda-bind time, not call-thread. Set-field updates on GitHub add one HTTP pre-fetch per edit — measured against `tracker_label_edit` scenario at slice boundary; expected `< 200 ms` total (network-bound, off-thread).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: every new HTTP path preserves existing 5s connect / 15s overall timeout. All triage + tracker-sync call sites stay worker-thread. No new UI-thread entry points.
- **Pillar 3 (never crash)**: standard error-handling discipline; sanitizer build via `ninja-test-msys2`. Interface deletion (`IGitHubReadClient`) is mechanical — type errors at compile time catch any miss. Audit-trail migration is additive with default — no NULL-bomb risk on legacy rows.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: Preferences UI changes reuse existing widget conventions; one less control after `AgenticPollSource` removal.

## Perf-review-system gates

Per [`docs/design/pillar-1-2-perf-review-system.md`](pillar-1-2-perf-review-system.md). Diff touches `Source_Core/` — gates apply.

1. **PR-fast CI** — primary: `tracker_sync` (GitHub backend); secondary: `agentic_triage_poll` (refactored). Add `tracker_label_edit` to track GitHub set-field pre-fetch overhead.
2. **Pillar 2 static scanner** — no new sync-I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — no `MainThreadDispatcher::Drain()` touch.
4. **Visible-cue bucket-E harness** — no new sync stalls > 100 ms.
5. **Marker inventory** — no new `SMATCHET_UI_PERF_SCOPE` markers planned.

**Pre-push local check**: `bash scripts/dev/perf-run.sh tracker_sync` + `agentic_triage_poll` + `tracker_label_edit`. `MISSING_BASELINE` acceptable on first run.

**Override**: `perf-out-of-band` PR label per [`AGENTS.md`](../../AGENTS.md) § Merge gates — not expected.

## Risks / non-goals

**Risks**:

- **Triage regression on GitHub** — highest risk. Mitigation: bundled PR (decision 6) means GitHub triage never runs against stubbed `UpdateField`; bucket-B scenario asserts pre-refactor behavior preserved.
- **Audit-trail migration on existing user DBs** — `ALTER TABLE … ADD COLUMN actor TEXT DEFAULT 'user'` is SQLite-safe but adds latency on first open of a large `agent_audit_trail`. Mitigation: migration runs in `AgentProposalStore` init on the worker thread; no UI freeze.
- **Plane `AddIssueCommentPlain` gap** — closes in A.8.
- **Jira `ListIssueKeysUpdatedSince` JQL shape divergence** — per-backend implementation in each `*IssueSearch.cpp`; the virtual returns canonical issue-keys + uses backend-native time format internally.
- **Two `GitHubClient` instances** — naive wire-in creates two PAT-holding clients. Mitigation per B.4 — `AppController::GetGithubClient()` is the single owner; factory returns a forwarding shell.
- **`SMATCHET_WITH_AGENTIC` un-gating** — drops triage-only code into vanilla builds (~150 LOC). Accepted; runtime PAT-empty short-circuit means dead-code at zero cost.
- **GitHub PATs vs OAuth** — PAT only. `TrackerConfig::GitHubBaseUrl` accommodates GitHub Enterprise. OAuth deferred.
- **GitHub Projects v2 custom fields** — phase ships native fields only. § Out of scope.
- **GitHub rate limits** — 5000/hr PAT-authed; GitHub `UpdateField("labels", ...)` adds one GET pre-fetch per label edit. At realistic triage rates (< 10/hr) the overhead is negligible.

**Non-goals**:

- **PR + check-run + CodeRabbit GraphQL surface** — stays GitHub-only (decision 1 § Code host).
- **Projects v2 custom fields via GraphQL** — separate plan.
- **GitHub Issues rich-text comment editor on grid** — Jira/Plane-only this phase.
- **PR-as-issue tracking** — tracker stays issues-only.
- **GitHub Enterprise OAuth / SSO** — PAT only.
- **Migration tooling** — no cross-backend issue migration.
- **Bucket-E coverage for Preferences-UI tracker switch** — deferred. `docs/backlog/agent-self-improvement/test.md` entry (category `test`) created in this PR.

## Verification

Per [`AGENTS.md`](../../AGENTS.md) § Verification automation — zero manual steps.

- **Bucket A (pure-logic ctest, `test-rig`)**:
  - `LabelEditDiffPure.test.cpp` — exhaustive set-diff.
  - `TrackerTriageDispatch.test.cpp` — mock `ITrackerClient` across "github" / "jira" / "plane"; assert dispatch + `sourceTracker` field correctness.
  - `GitHubClient_FieldCatalog.test.cpp` — static catalog shape.
  - `GitHubClient_UpdateField.test.cpp` — router dispatch + **internal pre-fetch + diff** asserted; **no field-id path makes a raw HTTP call outside the primitive set**.
  - `GitHubFieldCatalog.test.cpp` — `ResolveDisplayValue`.
  - `GitHubClientHelpers.test.cpp` (extend) — new helper round-trips.
  - `PlaneClient_AddComment.test.cpp` — closes A.8 gap.
  - `JiraClient_ListUpdatedSince.test.cpp` — JQL shape.
  - `AuditTrail_Actor.test.cpp` — default + explicit-actor round-trip + migration apply.
- **Bucket B (scenario runner)**:
  - `MultiTrackerTriageSmoke.cpp` — triage controller against three canned `ITrackerClient` mocks; asserts `agent_proposals` rows + `sourceTracker` field per source. CI-runnable (no GL context).
- **Bucket E**: deferred. Backlog entry covers Preferences-UI tracker-switch flow.
- **Build gate**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — dual-target.
- **Sanitizer gate**: `cmake --build --preset ninja-test-msys2` — ASan/UBSan clean.
- **Perf gate**: `bash scripts/dev/perf-run.sh tracker_sync agentic_triage_poll tracker_label_edit`.
- **Manual residue**: live-PAT smoke (one per backend) at first end-to-end run. Same gate Jira/Plane have today.

## Out of scope (flagged, not designed)

- **Projects v2 custom fields** — follow-up `docs/design/github-projects-v2-fields.md`.
- **Bitbucket / GitLab code-host integrations** — extends the code-host axis (decision 1 § Code host); not generalized at `ITrackerClient`.
- **GitHub Apps + OAuth** — PATs cover immediate need.
- **Repo-multi-select on a single tracker profile** — one `owner/repo` per profile.
- **GitHub-side audit-trail consumer** — existing `BackendAuditTrail` captures all writes.
- **Bucket-E coverage for Preferences-UI tracker switch** — backlog entry (`test` category).

## Implementation log
*(populated post-ship per [`AGENTS.md`](../../AGENTS.md) § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
