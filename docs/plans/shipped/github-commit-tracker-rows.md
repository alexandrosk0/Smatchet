# Plan - GitHub commit tracker rows
<!-- plan-date: 2026-05-28 -->

> **Slug**: `github-commit-tracker-rows`.
>
> **Origin**: user request, 2026-05-28: extend the GitHub client so commits are shown as tracker rows, not in a separate panel or one-off API.
>
> **Mandatory rules cross-link**: see `AGENTS.md` Project rules: Plan location, Plan-doc safety, Plan revision after implementation, Plan stress-test, Plan template, and Plan-doc perf-gate section.

## Context

The GitHub backend currently maps issues and pull requests into `CachedTicket` rows. `GitHubIssueSearchMapping.cpp` maps GitHub issue / PR JSON into `CachedTicket`, `GitHubIssueSearch.cpp` fetches pages through the GitHub API, and `GitHubClient::FetchIssuesStreamed` emits pages into the existing sync pipeline.

The desired outcome is: GitHub commits from the configured repository can appear in the same tracker grid as issues and PRs. A commit row is cached, sorted, filtered, opened in browser, and selected like any other tracker row, but it is read-only because Git commits are immutable history objects, not editable tracker issues.

Grill-with-docs pass:

- `docs/CONTEXT.md` currently defines `TrackerIssueKey` as an issue identifier, with GitHub shape `owner/repo#N`. Commit rows need a separate GitHub-specific key, not a redefinition of `TrackerIssueKey`.
- ADR 0003 already accepts GitHub as an `ITrackerClient` backend. This plan stays inside that decision: commit rows are an additive GitHub row kind within the tracker backend, not a new GitHub abstraction.
- No new ADR is needed. The decision is surprising enough to document in this plan, but it is reversible and contained to GitHub row mapping / fetch logic.

## Approach

Add commit rows as a GitHub-specific row kind in the existing `CachedTicket` stream. Issues and PRs keep their current `owner/repo#N` ids. Commits use a new key shape: `owner/repo@<full-sha>`. A helper parses this key only in GitHub code paths; generic tracker mutation contracts continue to treat ids as opaque strings.

Commit rows are selected by GitHub view query type. Existing issue-only behavior remains the default. Add:

- `type:commit` / `type = "commit"`: fetch commit rows only.
- `type:all` / `type:any`: fetch issues, PRs, and commits as rows.
- Existing `type:pr` and `type:issue` behavior stays compatible.

The first slice fetches commits only for the configured `GitHubOwner` + `GitHubRepo`; cross-repo commit search is out of scope. Commit fetch uses `GET /repos/{owner}/{repo}/commits?per_page=100` against the repository default branch. The initial window is the most recent 100 commits; older history is deliberately out of the first slice to keep sync cost and cache churn bounded.

## Files to modify

1. `Source_Core/include/GitHubClientHelpers.h` and `Source_Core/src/GitHubClientHelpers.cpp`: add `ParsedCommitKey`, `ParseGitHubCommitKey`, and commit URL suffix helpers.
2. `Source_Core/include/GitHubIssueSearchMapping.h` and `Source_Core/src/GitHubIssueSearchMapping.cpp`: add `MapCommitJsonToCachedTicket`; set `github.kind` on issue, PR, and commit rows.
3. `Source_Core/src/GitHubIssueSearch.h` and `Source_Core/src/GitHubIssueSearch.cpp`: add commit-row fetch orchestration beside the existing issue/PR fetch; preserve per-page streaming.
4. `Source_Core/include/GitHubFetchPlan.h` and `Source_Core/src/GitHubFetchPlan.cpp`: extend the plan from `includePullRequests` to row-source selection: issues, PRs, commits.
5. `Source_Core/include/GitHubQueryFromJql.h` and `Source_Core/src/GitHubQueryFromJql.cpp`: recognize `type:commit`, `type:all`, and `type:any`; update warnings for unknown type values.
6. `Source_Core/include/GitHubClient.h` and `Source_Core/src/GitHubClient.cpp`: expose commit fields in the static catalog, support commit browse URLs, mark commit rows read-only in editmeta, and reject mutations against commit keys.
7. `tests/Source_Core/GitHubClientHelpers.test.cpp`: add commit key parser and commit URL helper coverage.
8. `tests/Source_Core/GitHubIssueSearchMapping.test.cpp`: add commit JSON to `CachedTicket` mapping coverage.
9. `tests/Source_Core/GitHubFetchPlan.test.cpp`: add row-source selection coverage.
10. `tests/Source_Core/GitHubQueryFromJql.test.cpp`: add `type:commit` and `type:all` translation coverage.

No config schema bump is planned. The first slice has no new persisted setting.

## Row contract

Commit row mapping:

- `CachedTicket.id`: `owner/repo@<full-sha>`.
- `fieldValues["key"]`: same as `id`.
- `fieldValues["github.kind"]`: `commit`.
- `fieldValues["summary"]`: `[commit <short-sha>] <first commit-message line>`.
- `fieldValues["description"]`: full commit message, capped to the existing GitHub body cap.
- `fieldValues["status"]`: `commit`.
- `fieldValues["author"]` and `fieldValues["reporter"]`: GitHub author login when present, otherwise commit author name.
- `fieldValues["created"]`: commit author date.
- `fieldValues["updated"]`: committer date.
- `fieldValues["commit.sha"]`, `commit.short_sha`, `commit.author_name`, `commit.author_email`, `commit.committer_name`, `commit.committer_email`, `commit.url`, `commit.parents`, `commit.verified`: GitHub-specific columns.

Issue / PR rows also get `github.kind`:

- Issue: `issue`.
- Pull request: `pull_request`.

Mutation behavior:

- `GitHubClient::FetchIssueEditMeta` returns success with all fields false for commit keys.
- `GitHubClient::UpdateField` and `UpdateIssueFields` reject commit keys with a clear read-only error.
- Offline field replay should never enqueue against commit rows once editmeta is honored; the backend rejection is still the defensive last line.

Browse behavior:

- `BuildBrowseUrl` keeps `owner/repo#N` issue / PR behavior.
- `BuildBrowseUrl` returns `https://github.com/<owner>/<repo>/commit/<sha>` for `owner/repo@sha`.
- GitHub Enterprise keeps the current base-url host conversion rule.

## Existing utilities reused

- `CachedTicket` in `Source_Core/include/CachedTicketTypes.h`: the row payload and cache format already supports arbitrary field ids keyed by `ticket.id`.
- `LocalCacheManager::SaveTicket` in `Source_Core/src/LocalCacheManager.cpp`: no schema change is needed because ticket ids and fields are strings.
- `TicketSyncService::StartStreamingSync` in `Source_Core/src/TicketSyncService.cpp`: already tracks keep ids across streamed batches for stale pruning.
- `GitHubIssueSearchMapping` pure mapping helpers: extend the existing JSON-to-row test seam instead of adding HTTP-linked tests.
- `GitHubQueryFromJql`: keep view-query control in the existing GitHub translation layer.
- `GitHubFetchPlan`: keep source selection in the existing pure fetch-plan seam.
- `BuildGitHubHeaders`: reuse the current GitHub auth and API-version headers for commit REST calls.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: commit fetch runs on the existing sync worker path. The first slice caps commit rows at 100 to avoid large initial cache floods.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no HTTP or filesystem calls move to UI code. Commit fetch stays under `FetchIssuesStreamed` worker execution.
- **Pillar 3 (never crash)**: all JSON mapping is tolerant of missing `author`, `committer`, `verification`, `parents`, and malformed SHA fields; bad rows are skipped or mapped to safe defaults.
- **Pillar 4 (accessibility - keyboard nav / font scaling / WCAG AA)**: no new UI surface in the first slice. Commit fields appear through the existing grid and field picker.

## Perf-review-system gates

1. **PR-fast CI**: `priority-grid-scroll` is the closest existing scenario because commit rows enter the same sync-to-grid path as issue rows. No new perf scenario is required in the first slice.
2. **Pillar 2 static scanner**: fires. The plan adds a new HTTP endpoint, but it is reachable only from tracker sync worker code, not from `ImGui::*`.
3. **Dispatcher drain**: N/A. No `MainThreadDispatcher::Drain()` changes.
4. **Visible-cue bucket-E harness**: N/A. No new UI-thread stall path.
5. **Marker inventory**: N/A. No new `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: `bash scripts/dev/perf-run.sh priority-grid-scroll`.

## Risks / non-goals

- **Commit rows are not true issues**: mitigated with `github.kind`, commit-key parsing, read-only editmeta, and mutation rejection.
- **Existing `TrackerIssueKey` language is issue-only**: accepted for the first implementation. After the feature ships, update `docs/CONTEXT.md` with a `GitHub commit row key` glossary entry.
- **Stale pruning across mixed row sources**: `FullSyncCompleted` must be true only when every requested row source completed its bounded fetch. If the commit endpoint fails, return a warning and `FullSyncCompleted=false` so cached rows are not pruned from a partial result.
- **The 100-commit window is not all history**: accepted. This keeps the first slice small and deterministic. A later config or query extension can add `commit.limit` / `commit.ref`.
- **JQL clauses do not fully map to commits**: accepted. First slice uses `type` only for commit source selection. Issue-centric filters such as `assignee` continue to apply to issue/PR search, not to commit fetch.
- **Cross-repo commit search**: out of scope. Requires `/search/commits` semantics and different rate-limit behavior.
- **Commit-to-PR association**: out of scope. The first slice shows commits as rows; linking commits back to PRs can be a follow-up.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**:
  - `GitHubClientHelpers.test.cpp`: parse `owner/repo@sha`, reject malformed keys, compose commit browse path suffix.
  - `GitHubIssueSearchMapping.test.cpp`: map complete commit JSON, missing nested author/committer objects, parent list, and verification fields.
  - `GitHubFetchPlan.test.cpp`: preserve existing issue/PR behavior; add commit-only and all-row source selection.
  - `GitHubQueryFromJql.test.cpp`: `type:commit`, `type = "commit"`, `type:all`, `type:any`, and unknown type warnings.
- **Bucket B (scenario)**: N/A for first slice. Existing grid scenarios exercise row rendering once rows are cached.
- **Bucket E (ImGui Test Engine)**: N/A for first slice. No new UI control is added.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`.
- **Sanitizer gate**: `cmake --build --preset ninja-test-msvc`.
- **Aggregate gate**: `bash scripts/dev/test-all.sh`.
- **Manual residue**: none planned.

## Out of scope (flagged, not designed)

- User-configurable commit ref / branch.
- User-configurable commit limit.
- Cross-repo commit search.
- Commit-to-PR association fields.
- Commit diff / file list expansion.
- Separate commit detail panel.
- Editing commit rows or attaching offline field edits to them.

## Implementation log

- `74b1708e` · #504 (2026-05-28) - commit rows as a GitHub row kind: commit-key parser + URL helpers, `MapCommitJsonToCachedTicket`, REST commit fetch orchestrated beside the GraphQL issue/PR search, `type:commit`/`all`/`any` JQL, read-only editmeta + mutation rejection, commit browse URLs, commit catalog fields, and bucket-A tests across all four pure-logic seams.

## Deviations from plan

- **Issue/PR fetch is GraphQL, not REST `/issues`** (pre-existing Strategy C from PR12). Commits therefore land via a **separate REST path** (`GET /repos/{o}/{r}/commits?per_page=100`) orchestrated *beside* the GraphQL `search()` loop inside `FetchIssuesViaRestApi`, via two new anon-namespace helpers `RunGraphQlIssueSearch` + `RunCommitFetch`. Plan files-item 3/4 intent ("commit-row fetch beside the existing issue/PR fetch; preserve per-page streaming") is satisfied; the endpoint split differs from the plan's REST-only mental model. The terminal `isLast` streaming emission is owned by whichever source runs last.
- **Row-source selection modeled as three booleans, not one enum.** `JqlToGitHubResult` + `GitHubFetchPlan` gained `IncludeIssuesOrPullRequests`, `IncludePullRequests` (keep-PR-rows, distinct from the existing `IsPullRequestQuery` which drives `is:pr` injection), and `IncludeCommits`. `ComputeGitHubFetchPlan` downgrades `includeCommits`→false + warns when Owner+Repo aren't both set (cross-repo commits out of scope). `type:all`/`type:any` keep PRs without injecting `is:pr` so plain issues stay in the result.
- **`github.kind` exposed as a catalog field** ("Kind") in addition to the per-row value, so the field picker can surface the row-kind discriminator.
- **`commit.parents` stored as comma-joined short SHAs** (7-char); `commit.verified` as `"true"`/`"false"`/`""`. Commit-key SHA validation accepts 7–40 hex chars (abbreviated through full object id).
- **`FullSyncCompleted` guarded against the zero-source case** (commits-only view with no Owner/Repo downgrades to nothing) so a spurious full-sync never prunes the cache against an empty result.

## Verification (actual)

- **Bucket A (ctest doctest)**: PASSED — `ctest` on `ninja-test-msvc`, 842 test cases / 5765 assertions, 100% pass. New coverage: commit-key parse/reject + commit URL helpers (`GitHubClientHelpers.test.cpp`), full/partial/missing-nested commit mapping + `github.kind` stamping (`GitHubIssueSearchMapping.test.cpp`), row-source selection + commit downgrade (`GitHubFetchPlan.test.cpp`), `type:commit`/`all`/`any` translation + unknown-type warning (`GitHubQueryFromJql.test.cpp`).
- **Build gate**: PASSED — `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target, exit 0).
- **Sanitizer/test gate**: PASSED — `ninja-test-msvc` build exit 0.
- **clang-format**: PASSED — clean over all 14 changed files.
- **Bucket B / E**: N/A first slice (no new UI control).
- **Manual residue**: none.
