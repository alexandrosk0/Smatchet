# Plan — GitHub as a third tracker backend

> **Slug**: `github-tracker-backend`
>
> **Mandatory rules cross-link**: see [`AGENTS.md`](../../AGENTS.md) § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.
>
> **Stress-test pass**: 2026-05-21 grill-with-docs + 2 architect passes. **2026-05-21 scope reduction**: agentic-triage tracker-agnostic refactor stripped from this plan; deferred to a future plan. This plan now ships only GitHub-as-grid-tracker; agentic flow stays untouched (GitHub-only, current behaviour).

## Context

Smatchet today ships two grid-backing tracker backends — Jira and Plane — wired via `ITrackerClient` and `DefaultTrackerBackendFactory`. A third surface, GitHub, already exists in-tree as [`GitHubClient`](../../Source_Core/include/GitHubClient.h) but is currently **triage-only**: it powers the agentic-flow pipeline (CodeRabbit + CI react loop, `handoff-implementer`, `pr-iterator`, the T7 scheduled-poll worker) and stubs out every grid-relevant `ITrackerClient` virtual with the documented "not supported on GitHub backend yet" sentinel.

User ask: let users pick **GitHub** as the active tracker so issues from a configured `owner/repo` populate the grid, sync into the SQLite cache, and support field-edit + create-issue flows the same way Jira/Plane do today.

**Out of scope for this plan**: making the agentic-triage pipeline tracker-agnostic (i.e. letting Jira / Plane issues drive triage proposals). The triage seam stays GitHub-only as-is. A future plan will refactor it; this plan does not block on or include that work. See § Out of scope § Agentic triage tracker-agnostic refactor.

**Anti-duplication constraint**: the existing `GitHubClient` triage primitives — `CommentAdd` / `LabelAdd` / `LabelRemove` / `AssigneeSet` / `StateTransition` / PR-thread / check-run / GraphQL-resolve — must stay the single source of truth for GitHub writes. The tracker role **routes** through those primitives via `UpdateField`; it does not parallel them.

Cross-link: [ADR 0003](../adr/0003-github-as-itrackerclient.md) (GitHub as ITrackerClient was the originally-decided shape); [`docs/design/agentic-flow-implementation.md`](agentic-flow-implementation.md) (the triage-side contract this plan must not regress).

## Decisions locked

Locked by the 2026-05-21 grill-with-docs + architect-review sessions. Diverging from any of these = needs a plan revision, not an implementation choice.

1. **`UpdateField` semantics = set-replace at the virtual** — `values` is the intended full set after edit. Jira / Plane native; GitHub impl pre-fetches the current set and diffs internally. CONTEXT.md § `UpdateField` semantics — set-replace.
2. **Factory forwarding-shell shape pinned** — `DefaultTrackerBackendFactory` gains ctor-injected `AppController*`; `Create("github")` returns `unique_ptr<GitHubForwardingClient>` shell holding lazily-resolved non-owning `GitHubClient* impl_`. Single shared `GitHubClient` instance owned by `AppController`; no second PAT-holding client.
3. **`SMATCHET_WITH_AGENTIC` un-gates `GitHubClient*.cpp`** — runtime PAT-empty short-circuit replaces build-time gating; symbol must exist in tracker-only builds. Header preprocessor audit confirms no `#error` branches remain. Dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) is a hard gate before commit.
4. **GitHub field catalog is static, no API** — six native fields (state, labels, assignees, milestone, title, body). Projects v2 custom fields deferred to a follow-up plan.
5. **Single bundled PR** — all GitHub fill-in landings ship together. No intermediate state where the factory wires `"github"` but the virtuals are still stubbed.
6. **Verification = Bucket A pure-logic only** — router dispatch, set-diff helper, static catalog, helper round-trips. No bucket-B / bucket-E in this PR; tracker-switching UI coverage is a backlog entry.

## Approach

Single work-stream, single squashed PR. The existing `GitHubClient` keeps its triage-only role exactly as-is; this plan only fills the stubbed `ITrackerClient` virtuals so the grid + sync paths recognise GitHub as a valid backend.

**1. De-gate the TU** — drop `#if defined(SMATCHET_WITH_AGENTIC)` around the `GitHubClient` source list and `GitHubPat` config field. Runtime PAT-empty short-circuit means dead-code at zero cost without configuration.

1.a **Header preprocessor audit** — `GitHubClient.h` lines 10-12 currently document the build-time-gating contract for consumers. Every `#if defined(SMATCHET_WITH_AGENTIC)` inside `GitHubClient.h` / `GitHubClient.cpp` / `GitHubClientHelpers.h` / `GitHubClientHelpers.cpp` either drops or converts to a runtime guard. No `#error` branch may remain.

1.b **`GitHubClientHelpers.cpp` un-gates alongside `GitHubClient.cpp`** — `CMakeLists.txt:654-655` + `:678-680` currently `REMOVE_ITEM` + re-add both files in the same gated block. The un-gate must cover both TUs together.

1.c **Dual-target build is a hard gate** — `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` MUST pass before commit. DX12 builds with `SMATCHET_WITH_AGENTIC=OFF` (per `CMakeLists.txt:175`); confirm `GitHubClient.cpp`'s PR / GraphQL / check-run methods compile cleanly under that flag.

**2. Implement the stubbed `ITrackerClient` virtuals on `GitHubClient`** — `FetchIssues`, `FetchIssuesForKeys`, `ProbeReachability`, `BuildBrowseUrl`, `ExtractProjectFromQuery`, `ListProjects`, `FetchFieldCatalog` (static catalog, no API), `ResolveDisplayValue`, `UpdateIssueFields`, `UpdateField`, `BuildFieldPayload`, `BuildCreatePayload`, `CreateIssue`.

The write virtuals dispatch internally to existing five primitives plus one new shared `PatchIssue` helper for `title` / `body` / `milestone` (the three GitHub fields sharing the `PATCH /repos/{o}/{r}/issues/{n}` endpoint). `UpdateField` for set fields (labels, assignees) does an internal pre-fetch + diff per decision 1:

```
UpdateField(issueId, "labels", desiredSet):
    parse issueId via ParseGitHubIssueKey
    GET /repos/{o}/{r}/issues/{n} → currentLabels
    {toAdd, toRemove} = ComputeLabelEditDiff(currentLabels, desiredSet)
    for each in toAdd: LabelAdd(...)
    for each in toRemove: LabelRemove(...)
    audit-trail one "field_update_labels" row spanning the batch (begin/result)
```

**3. Wire the factory + config** — extend `DefaultTrackerBackendFactory::Create` with a `"github"` branch; add `GitHubBaseUrl`, `GitHubOwner`, `GitHubRepo` to `TrackerConfig` (PAT already there); extend `SmatchetPreferencesUi` with the GitHub profile group.

**4. Single shared `GitHubClient` instance** — promote `AppController::agenticGithubClient_` to `sharedGithubClient_`; both triage and tracker call sites resolve through `AppController::GetGithubClient()`.

**Factory shape**: `DefaultTrackerBackendFactory` gains a non-owning `AppController* appController_` member (ctor-injected; existing call site `AppController.cpp` that instantiates the factory passes `this`). `Create("github")` returns `unique_ptr<GitHubForwardingClient>` where `GitHubForwardingClient` is a new TU-private class inside `DefaultTrackerBackendFactory.cpp` holding a non-owning `GitHubClient* impl_`. Every `ITrackerClient` virtual on the shell **lazily** resolves `impl_ = appController_->GetGithubClient()` on first virtual call (not at shell construction); avoids the ctor-order trap where `sharedGithubClient_` is not yet initialised when the factory is built during `AppController` ctor. The shell's default destructor only deletes the shell — the AppController retains sole ownership of the underlying `GitHubClient`. Rejected alternative: `unique_ptr<ITrackerClient, NoopDeleter>` — breaks the default-deleter contract every `unique_ptr<ITrackerClient>` consumer expects.

Non-obvious trade-off: GitHub Issues has very few native fields. Projects v2 custom fields (GraphQL) are a separate effort. Phase 1 ships native fields only.

## Files to modify

Grouped by subsystem; one squashed PR.

**GitHubClient surface fill**

1. [`Source_Core/include/GitHubClient.h`](../../Source_Core/include/GitHubClient.h:35) — drop "only `FetchIssueComments` is real this slice" comment; un-stub virtual declarations; add private `PatchIssue` helper.
2. [`Source_Core/src/GitHubClient.cpp`](../../Source_Core/src/GitHubClient.cpp:107) — replace stub bodies for `ProbeReachability` / `FetchIssues` / `FetchIssuesForKeys` / `UpdateIssueFields` / `UpdateField` / `BuildFieldPayload` / `ResolveDisplayValue`; add `BuildBrowseUrl`, `ExtractProjectFromQuery`, `ListProjects`, `FetchFieldCatalog`, `BuildCreatePayload`, `CreateIssue` overrides; add private `PatchIssue`. `UpdateField` routes by `field.id` → existing primitives + diff-via-`LabelEditDiffPure` for set fields.
3. **New file** [`Source_Core/src/GitHubIssueSearch.cpp`](../../Source_Core/src/) — paginated `FetchIssues` + `FetchIssuesForKeys` impl. Mirrors `JiraIssueSearch.cpp` / `PlaneIssueSearch.cpp` split.
4. **New file** [`Source_Core/src/GitHubFieldCatalog.cpp`](../../Source_Core/src/) — static catalog (6 fields) + `ResolveDisplayValue`. Mirrors `PlaneFieldCatalog.cpp`.
5. [`Source_Core/include/GitHubClientHelpers.h`](../../Source_Core/include/GitHubClientHelpers.h) + [`.cpp`](../../Source_Core/src/GitHubClientHelpers.cpp) — add `BuildIssuePatchSuffix`, `BuildIssuesListSuffix`.
6. **New pure helper** [`Source_Core/include/LabelEditDiffPure.h`](../../Source_Core/include/) + [`Source_Core/src/LabelEditDiffPure.cpp`](../../Source_Core/src/) — `ComputeLabelEditDiff(currentLabels, intendedLabels) → {toAdd, toRemove}`. Pure, no I/O, bucket-A testable. Used by GitHub `UpdateField` internal diff.

**Factory + config**

7. [`Source_Core/src/DefaultTrackerBackendFactory.cpp`](../../Source_Core/src/DefaultTrackerBackendFactory.cpp:7) — `"github"` branch; ctor takes `AppController*`; `GitHubForwardingClient` private class with lazy `impl_` resolution.
8. [`Source_Core/include/DefaultTrackerBackendFactory.h`](../../Source_Core/include/) — factory ctor signature change.
9. [`Source_Core/include/ConfigManager.h`](../../Source_Core/include/ConfigManager.h:222) — un-gate `GitHubPat` (drop `#if SMATCHET_WITH_AGENTIC`); add `GitHubBaseUrl`, `GitHubOwner`, `GitHubRepo`.
10. [`Source_Core/src/ConfigManager.cpp`](../../Source_Core/src/ConfigManager.cpp) — Load / Save the three new fields; legacy-config migration.
11. [`Source_Core/src/SmatchetPreferencesUi.cpp`](../../Source_Core/src/SmatchetPreferencesUi.cpp) — GitHub profile group under Tracker section.
12. [`Source_Core/include/AppController.h`](../../Source_Core/include/AppController.h:1276) — rename `EnsureAgenticGithubClient` → `GetGithubClient`; rename member `agenticGithubClient_` → `sharedGithubClient_`. Factory call site updated to pass `this`.
13. [`Source_Core/src/AppController.cpp`](../../Source_Core/src/AppController.cpp:1655) — rename uses; agentic-flow lambdas continue to resolve through `GetGithubClient()` (same instance now); existing triage write lambdas at lines 1894-2036 unchanged in shape.

**Build glue**

14. [`CMakeLists.txt`](../../CMakeLists.txt) — drop `SMATCHET_WITH_AGENTIC` from `GitHubClient.cpp` + `GitHubClientHelpers.cpp` source-list condition (lines 654-655 + 678-680). The agentic-flow-only test TUs (`GitHubClient_PrSurface.test.cpp`, `GitHubClient_GraphQL.test.cpp`) stay gated separately.

**Tests (Bucket A — pure-logic ctest)**

15. [`tests/Source_Core/LabelEditDiffPure.test.cpp`](../../tests/Source_Core/) — exhaustive set-diff cases.
16. [`tests/Source_Core/GitHubClient_FieldCatalog.test.cpp`](../../tests/Source_Core/) — static catalog shape: 6 fields, allowed values, types.
17. [`tests/Source_Core/GitHubClient_UpdateField.test.cpp`](../../tests/Source_Core/) — field-router dispatch: state → `StateTransition`, labels → diff via `LabelEditDiffPure` → `LabelAdd` / `LabelRemove`, assignee → `AssigneeSet`, title / body / milestone → `PatchIssue`. Mocked HTTP per existing `GitHubClient_GraphQL.test.cpp` pattern. **Critical assertion: the GET-current-labels pre-fetch is asserted on every set-field update; no field-id path makes a raw HTTP call outside the primitive set.**
18. [`tests/Source_Core/GitHubFieldCatalog.test.cpp`](../../tests/Source_Core/) — `ResolveDisplayValue` for assignees + labels.
19. [`tests/Source_Core/GitHubClientHelpers.test.cpp`](../../tests/Source_Core/GitHubClientHelpers.test.cpp) — extend with `BuildIssuePatchSuffix`, `BuildIssuesListSuffix`, `ExtractProjectFromQuery`.
20. [`tests/Source_Core/GitHubForwardingClient.test.cpp`](../../tests/Source_Core/) — factory shell lazy `impl_` resolution: assert first virtual call triggers `GetGithubClient()`, subsequent calls reuse, null AppController returns safe sentinel.

## Existing utilities reused

Anti-duplication contract.

- `GitHubClient::CommentAdd` + `LabelAdd` + `LabelRemove` + `AssigneeSet` + `StateTransition` — [GitHubClient.h:134-168](../../Source_Core/include/GitHubClient.h:134) — `UpdateField` is a router; no new HTTP path for state/labels/assignees/comments.
- `GitHubClient::ListOpenIssuesForRepo` `since=` cursor + per_page cap — promoted to `GitHubClientHelpers::BuildIssuesListSuffix` so list + tracker-sync share parameter encoding.
- `GitHubClient::FetchIssueBody` — already on the agentic seam; left in place. Tracker fetch path does not use it (uses `FetchIssues` instead).
- `MakeGitHubAuthHeaders` + `ComposeHttpErrorString` + `RedactForLog` — [GitHubClient.cpp:38-69](../../Source_Core/src/GitHubClient.cpp:38) — every new HTTP call uses these. Per [`docs/design/agentic-flow-implementation.md`](agentic-flow-implementation.md) § Decisions locked #2, the inline-bearer pattern stays.
- `GitHubClientHelpers::ParseGitHubIssueKey` + `BuildIssue*Suffix` family — [GitHubClientHelpers.h](../../Source_Core/include/GitHubClientHelpers.h) — every new write parses keys through the same helper. Key shape stays `owner/repo#N`.
- `BackendAuditTrail::AppendBegin/AppendResult` with `source="github_client"` — unchanged; triage-flow audit-consumers keep working.
- `CachedTicket` + `LocalCacheManager` — pure-data shape used by Jira / Plane `FetchIssues`. GitHub `FetchIssues` produces the same struct.
- `TrackerFieldCatalogResult` + `TrackerField` — [`TrackerFieldSchema.h`](../../Source_Core/include/TrackerFieldSchema.h).
- `AppController::EnsureAgenticGithubClient` lazy + `std::call_once` ownership — renamed + reused; no second instance.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: tracker fetch + field edits stay off-UI per existing `ITrackerClient` contract. GitHub set-field updates add one HTTP pre-fetch per edit (label / assignee changes); measured against `tracker_label_edit` scenario at slice boundary; expected `< 200 ms` total (network-bound, off-thread).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: every new HTTP path preserves existing 5s connect / 15s overall timeout (`kGitHubConnectTimeoutMs` / `kGitHubOverallTimeoutMs`). All call sites stay worker-thread (`TicketSyncService`, field-edit pipeline). No new UI-thread entry points.
- **Pillar 3 (never crash)**: standard error-handling discipline — PAT-presence check first, parse-fail returns `false` + `outError`, no raw `new` / `delete`, JSON wrapped in `try` / `catch`. Sanitizer build via `ninja-test-msys2`. Factory lazy `impl_` resolution guards against null-deref via documented sentinel return.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no new UI widgets beyond the Preferences profile group; reuses existing `SmatchetPreferencesUi` conventions.

## Perf-review-system gates

Per [`docs/design/pillar-1-2-perf-review-system.md`](pillar-1-2-perf-review-system.md). Diff touches `Source_Core/` — gates apply.

1. **PR-fast CI** — primary scenario: `tracker_sync` (added GitHub backend path); secondary: `tracker_label_edit` (GitHub set-field pre-fetch overhead). Verify both are in `scripts/dev/perf-pr-fast-set.json`; if not, declare in PR body which scenario covers the change.
2. **Pillar 2 static scanner** — no new sync-I/O reachable from `ImGui::*`. All HTTP / parse work stays on worker threads.
3. **Dispatcher drain** — no `MainThreadDispatcher::Drain()` touch.
4. **Visible-cue bucket-E harness** — no new sync stalls > 100 ms; existing spinner / progress widgets cover the new code paths via shared call sites.
5. **Marker inventory** — no new `SMATCHET_UI_PERF_SCOPE` markers planned. If implementer adds any for tracker-fetch hot paths, regen `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push local check**: `bash scripts/dev/perf-run.sh tracker_sync` + `tracker_label_edit` + `perf-compare.py` against any existing baseline. `MISSING_BASELINE` acceptable on first run; CI auto-bootstraps.

**Override**: `perf-out-of-band` PR label per [`AGENTS.md`](../../AGENTS.md) § Merge gates — not expected.

## Risks / non-goals

**Risks**:

- **Two `GitHubClient` instances** — naive wire-in (factory builds its own, AppController keeps `agenticGithubClient_` for triage) creates two PAT-holding clients per process. Mitigation per decision 2 — `AppController::GetGithubClient()` is the single owner; factory returns a lazy-resolving forwarding shell.
- **`SMATCHET_WITH_AGENTIC` un-gating** — dropping the build gate adds the triage code path (PR/check-run/GraphQL) to the standalone binary even when the user doesn't use it. Binary-size impact: ~150 LOC of additional cpr call sites + helpers. Accepted — the runtime PAT-empty short-circuit means dead-code at zero cost without configuration.
- **DX12 dual-target compile risk** — un-gating un-gates the GitHubClient TU for the DX12 target which builds with `SMATCHET_WITH_AGENTIC=OFF`. Mitigation per decision 3 — dual-target build is hard gate; header preprocessor audit confirms no `#error` branches remain.
- **GitHub PATs vs OAuth** — current design keeps PAT-only. Mitigation: `TrackerConfig::GitHubBaseUrl` lets users point at GitHub Enterprise. OAuth is a follow-up plan.
- **GitHub Projects v2 custom fields** — phase 1 ships native fields only. Mitigation: § Out of scope flags the follow-up plan explicitly. Field catalog's static-builder leaves room to merge in a GraphQL-fetched Projects-v2 catalog later.
- **GitHub rate limits** — 5000/hr PAT-authed. `FetchIssues` paginating at 100/page comfortable; `UpdateField("labels", ...)` adds one GET pre-fetch per label edit. At realistic edit rates (< 10/hr) the overhead is negligible.
- **Agentic flow regression** — triage path continues calling `GitHubClient::CommentAdd` / `LabelAdd` / etc. directly. Mitigation: this plan does not touch the triage call sites. The shared-instance promotion (decision 2) is a rename, not a behaviour change. Bucket-A test `GitHubForwardingClient.test.cpp` asserts factory's shell uses the same underlying instance.

**Non-goals**:

- **Agentic triage tracker-agnostic refactor** — deferred. The agentic flow (CodeRabbit react loop, CI react loop, T7 scheduled-poll worker, AgenticTriageController) stays GitHub-only. A future plan will reshape `IGitHubReadClient` → backend-agnostic seam and rebind triage write lambdas; that work is independent of this plan.
- **PR + check-run + CodeRabbit GraphQL surface generalisation** — stays GitHub-only by construction; no Jira/Plane analog.
- **Projects v2 custom fields via GraphQL** — separate plan.
- **GitHub Issues rich-text comment editor on grid** — Jira/Plane-only this phase.
- **PR-as-issue tracking** — tracker stays issues-only.
- **GitHub Enterprise OAuth / SSO** — PAT only.
- **Migration tooling** — no cross-backend issue migration.

## Verification

Per [`AGENTS.md`](../../AGENTS.md) § Verification automation — zero manual steps.

- **Bucket A (pure-logic ctest, `test-rig`)**:
  - `LabelEditDiffPure.test.cpp` — exhaustive set-diff.
  - `GitHubClient_FieldCatalog.test.cpp` — static catalog shape.
  - `GitHubClient_UpdateField.test.cpp` — router dispatch + **internal pre-fetch + diff** asserted; **no field-id path makes a raw HTTP call outside the primitive set**.
  - `GitHubFieldCatalog.test.cpp` — `ResolveDisplayValue`.
  - `GitHubClientHelpers.test.cpp` (extend) — new helper round-trips.
  - `GitHubForwardingClient.test.cpp` — lazy `impl_` resolution; shell delegates to AppController-owned instance.
- **Bucket E**: deferred. Backlog entry covers Preferences-UI tracker-switch flow.
- **Build gate**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — dual-target. Must pass with `SMATCHET_WITH_AGENTIC=OFF` (no-agentic build still compiles `GitHubClient`).
- **Sanitizer gate**: `cmake --build --preset ninja-test-msys2` — ASan/UBSan clean.
- **Perf gate**: `bash scripts/dev/perf-run.sh tracker_sync tracker_label_edit`.
- **Manual residue**: live-PAT smoke at first end-to-end run. Same gate Jira/Plane have today. Test PATs in CI = security non-starter.

## Out of scope (flagged, not designed)

- **Agentic triage tracker-agnostic refactor** — `IGitHubReadClient` → backend-agnostic seam, `AgenticTriageController` gates lift, triage write-lambdas rebind through `ITrackerClient`, Plane `AddIssueCommentPlain` fill, `ListIssueKeysUpdatedSince` virtual addition, `AgenticPollSource` retirement, `HandoffClarificationPostToGithub` rename, audit-trail `actor` field. **Deferred to a future plan**; the design exploration for that refactor is captured in this plan's git history (commits `76c57d6c` / `e9eb0478` / `491f8425` / `7ae7e584`) for the next planner's reference. CONTEXT.md entries for `Source tracker` / `Code host` / `TrackerIssueKey` stay — those concepts hold whether or not the refactor lands.
- **Projects v2 custom fields** — follow-up `docs/design/github-projects-v2-fields.md`. Drops in by extending `FetchFieldCatalog` to merge a GraphQL `node(id: <project>) { fields { ... } }` result into the static native-field catalog.
- **Bitbucket / GitLab code-host integrations** — extends the code-host axis (CONTEXT.md § Code host); not generalized at `ITrackerClient`.
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
