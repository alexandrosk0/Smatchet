# Plan — GitHub pull requests in the tracker grid (PR12 of github-tracker-backend)
<!-- plan-date: 2026-05-22 -->

> **Slug**: `github-tracker-pr12-prs-in-grid`.
> **Status**: shipped — PR4 (`#387`) merged 2026-05-22; this PR opens against develop.
> **Originating prompt**: 2026-05-22 "can I see PRs?" during PR4 dogfood.
> **Parent plan**: [`docs/plans/shipped/github-tracker-backend.md`](github-tracker-backend.md) § Remaining for GitHub issues to work.

## Context

PR4 (`#387` — `feat(github-tracker): PR4 + PR5 — FetchIssues HTTP impl + JQL translator`) ships the GitHub `FetchIssues` HTTP path with an explicit filter that drops pull requests from the result set (`GitHubIssueSearch.cpp` skips items containing the `pull_request` field). This was correct for the issue-grid use case but rules out the legitimate workflow of viewing PRs as tracker items.

GitHub's REST API returns PRs and issues from the same `/repos/{o}/{r}/issues` endpoint — PRs are issues with an extra `pull_request` object attached. The `/search/issues` endpoint includes PRs only when the query carries `is:pr`. So the surface needs both a fetch-level relaxation (don't auto-drop PRs) and a view-level intent signal (user explicitly asked for PRs).

One-sentence outcome: after this lands, a user can write a Smatchet view query like `type:pr` or `type:issue` and see the matching subset in the grid, with PR-specific columns (head/base branch, mergeable, draft) populated alongside the existing issue columns.

## Decisions locked

| Concern | Decision |
|---|---|
| Gate | Per-view JQL token: `type:pr` / `type:issue`. Default (no token in the view's JQL) = issues only, matching today's behavior. No Preferences toggle. |
| Visual — distinguishing PR from Issue | Two channels combined: (a) badge prefix `[PR] ` injected into the `summary` field, (b) `status` field encodes the PR merge state with the values `open`, `closed`, `merged-PR`. |
| PR-only fields | Surface all four as new grid columns: `pr.head` (head branch), `pr.base` (base branch), `pr.mergeable` (bool flag), `pr.draft` (bool flag). |
| Storage | Reuse `CachedTicket::fieldValues` (`std::unordered_map<string, string>` per ticket — `Source_Core/include/CachedTicketTypes.h:15`). No new schema fields, no DB-side migration. |

## Approach

Three surface changes:

1. **JQL translator** — recognise the `type:` token in the source JQL and emit the GitHub equivalent. `type:pr` → append `is:pr` to the translated `q=`. `type:issue` → append `is:issue` (explicit; idempotent against GitHub's default). Default (no `type:` in source) — current behavior preserved.
2. **Fetch path** — `GitHubIssueSearch.cpp` no longer unconditionally drops items containing `pull_request`. Drop iff the view's translated query did NOT carry `is:pr`. The translator-output's effective query (already computed) is the signal.
3. **Field catalog + mapping** — `GitHubFieldCatalog.cpp` declares four new fields (`pr.head`, `pr.base`, `pr.mergeable`, `pr.draft`). `MapIssueToCachedTicket` in `GitHubIssueSearch.cpp` populates them when the item is a PR; leaves them empty for issues. Status mapping for PRs: `open` → `open`, `closed` AND `pull_request.merged_at` non-null → `merged-PR`, `closed` otherwise → `closed`.

Trade-off: the badge + status-merge-encoding pair is a soft visual contract — the user has to read both columns to identify "merged PR" vs "closed PR". An alternative would be a dedicated `type` column, but that adds a column to every backend's grid (Jira/Plane don't need it). Two-channel encoding keeps the per-backend customization in the value layer.

## Files to modify

Numbered list. Per-file rationale.

### Modified files

1. `Source_Core/include/GitHubQueryFromJql.h` — no signature change; documented `type:` token added to the supported-grammar block-comment.
2. `Source_Core/src/GitHubQueryFromJql.cpp` — token recognizer for `type:pr` / `type:issue` (case-insensitive). Emit `is:pr` / `is:issue` into the output `Query`.
3. `Source_Core/src/GitHubIssueSearch.cpp` — `pull_request` drop conditional on whether `is:pr` is in the effective query (passed through from `ComputeGitHubFetchPlan`). `MapIssueToCachedTicket` extended to populate four PR fields + adjust status + badge summary.
4. `Source_Core/src/GitHubFieldCatalog.cpp` — declare `pr.head`, `pr.base`, `pr.mergeable`, `pr.draft` as type=`string` (mergeable + draft are `"true"`/`"false"` strings consistent with existing bool patterns in this catalog).
5. `Source_Core/include/GitHubFieldCatalog.h` — only if the new field IDs need exported constants; check existing pattern (Jira uses literal strings inline — likely no header change).

### New tests

6. `tests/Source_Core/GitHubQueryFromJql.test.cpp` — add 6 cases:
   - `type:pr` alone → `is:pr`
   - `type:issue` alone → `is:issue`
   - `type:pr AND assignee = currentUser()` → `is:pr assignee:@me`
   - `TYPE:PR` (case-insensitive) → `is:pr`
   - No `type:` token → no `is:pr` / `is:issue` in output (preserve current behavior)
   - Unknown `type:foo` → drop with warning (don't propagate to GitHub)
7. `tests/Source_Core/GitHubFetchPlan.test.cpp` — extend to assert `effectiveQuery` carries `is:pr` through when the translator output had it.
8. `tests/Source_Core/GitHubIssueSearch_Mapping.test.cpp` — new bucket-A test for `MapIssueToCachedTicket` (will need to extract that helper out of the cpr-dependent TU, similar to the `GitHubFetchPlan` split shipped in the PR4 follow-up). Cases:
   - Plain issue → no `[PR]` prefix, no `pr.*` fields populated
   - Open PR → `[PR]` prefix, status=`open`, `pr.draft="false"`, `pr.mergeable="true"`, `pr.head`/`pr.base` set
   - Merged PR → status=`merged-PR`
   - Closed-without-merge PR → status=`closed`
   - Draft PR → `pr.draft="true"`

### Out of scope, explicitly **not** touched

- Jira / Plane backend behavior — `type:` token is GitHub-only at the translator layer. Jira's JQL already has `type` as a field; passing `type:pr` through to Jira would fail backend-side (no such issuetype). Translator detects the active backend via call-site context; the GitHub translator owns the token semantics.
- New grid view UI — the four PR columns live in the existing field catalog rendering path. They render iff the active view's `SelectedFields` includes them. Default view selection unchanged.
- Bucket-E coverage — deferred to the umbrella PR10 (bucket-E for tracker switch) per the parent plan's § Remaining list.

## Pillar callouts

- **Pillar 1 (perf, 144 Hz)**: N/A — adds ~6 string lookups per ticket map; negligible against the existing 4096-byte body copy.
- **Pillar 2 (UI never blocks > 100 ms)**: N/A — same fetch path, same worker thread.
- **Pillar 3 (never crash)**: PR mapping branches must guard against missing fields (`pull_request` shape isn't part of GitHub's stable `issue` schema — handle absence per existing helper `JsonString` pattern).
- **Pillar 4 (a11y)**: badge prefix `[PR]` is text-only — keyboard nav unaffected. Color contrast unchanged.

## Perf-review-system gates

N/A — diff touches `Source_Core/` but the changed code paths are not in any PR-fast scenario's hot path (translator + fetch + mapping run on background worker thread, not on `ImGui::*` frame stack).

## Risks / non-goals

- **Risk — view-query language drift**: introducing GitHub-specific `type:` token to the translator nudges Smatchet's view query syntax further from pure JQL. Mitigation: keep the token recognized only by the GitHub translator; Jira translator unchanged. Document the deviation in `docs/CONTEXT.md` § Source control glossary entries on next revision.
- **Risk — PR status conflation**: `merged-PR` as a status value is a Smatchet-side encoding, not a GitHub primitive. If the user filters views by `status = "closed"`, merged PRs do NOT match. Mitigation: filter-form documentation; possibly add `status in ("closed", "merged-PR")` example to view-editor help text.
- **Non-goal — GitHub Projects V2 integration**: PRs in the grid is not PRs in projects. Projects V2 is a separate GraphQL surface; out of scope per parent plan.
- **Non-goal — PR review state in the grid**: `pr.review_decision` (APPROVED / CHANGES_REQUESTED / REVIEW_REQUIRED) is interesting but adds REST round-trips; defer to a later PR if user demand surfaces.

## Verification

- **Bucket A (pure-logic ctest)**: new cases in `GitHubQueryFromJql.test.cpp` + new `GitHubIssueSearch_Mapping.test.cpp` per § Files to modify.
- **Bucket E (ImGui Test Engine)**: N/A — no UI shape change beyond column population.
- **Manual residue**:
  - Set view JQL to `type:pr`, save, observe PR rows with `[PR]` prefix + `merged-PR` status for landed PRs.
  - Set view JQL to `type:issue`, observe issues-only.
  - Default view (no `type:`), observe issues-only (regression gate — same as today).

## Implementation log

- `<sha-PR12a>` · `feat(github-tracker): PR12a — type: token in JQL translator + tests` — adds `IsPullRequestQuery` to `JqlToGitHubResult`, tokenises `:` as an Op so `type:pr` shorthand parses, accepts `type = "pr"` full JQL too, emits `is:pr` / `is:issue` accordingly. Tests in `tests/Source_Core/GitHubQueryFromJql.test.cpp` (+7 cases).
- `<sha-PR12b>` · `feat(github-tracker): PR12b — includePullRequests in fetch plan + tests` — adds `bool includePullRequests` to `GitHubFetchPlan`; `ComputeGitHubFetchPlan` gains a 4th `isPullRequestQuery` arg (default false) forwarded onto the plan flag. Tests in `tests/Source_Core/GitHubFetchPlan.test.cpp` (+3 cases).
- `<sha-PR12c>` · `feat(github-tracker): PR12c — surface PRs in fetch + per-PR enrichment + status encoding + 4 columns` — extracts JSON→CachedTicket mapping into a pure TU at `Source_Core/{include,src}/GitHubIssueSearchMapping.{h,cpp}` so doctest can link it without cpr; threads `includePullRequests` through the fetch loop (drops PR-skip filter when set); adds per-PR enrichment loop calling `GET /repos/{o}/{r}/pulls/{n}` for branch refs + mergeable + draft; adds 4 `pr.*` fields to the static catalog in `GitHubClient::FetchFieldCatalog`. Tests in `tests/Source_Core/GitHubIssueSearchMapping.test.cpp` (9 cases covering issue/PR detection, status encoding, enrichment).
- `<sha-PR12d>` · `docs(plan): revise github-tracker-pr12-prs-in-grid.md post-impl` — this revision.
- `<sha-PR12e>` · `perf(github-tracker): PR12 — replace REST N+1 with single GraphQL query per page` — replaces paginated REST `/search/issues` + per-PR `GET /pulls/{n}` (up to 1000 sync HTTP calls on the worker thread) with one `POST /graphql` per page (up to 10 calls total, N/100). Adds `MapGraphQlNodeToRestShape` + `MapGraphQlPullRequestNodeToRestPrShape` adapters in `GitHubIssueSearchMapping.{h,cpp}` so the existing `MapIssueOrPullRequestJsonToCachedTicket` + `EnrichPullRequestFieldsFromJson` pure-logic mappers stay unchanged. Adds 12 bucket-A tests in `GitHubIssueSearchMapping.test.cpp`.
- `<sha-PR12f>` · `perf(github-tracker): PR12 — per-page streaming apply (Slice 1 of latency-perception fix)` — `FetchIssuesViaRestApi` gains a second overload taking `std::function<void(const std::vector<CachedTicket>&, bool isLast)> onPage`; each GraphQL page emits its mapped tickets immediately (post sentinel-strip), so the grid populates progressively (≈ t+1.7s, t+3.4s, …) instead of all-at-once at t+6s. `GitHubClient::FetchIssuesStreamed` overrides the `ITrackerClient` default single-batch path and forwards each `onPage` invocation to the streaming-apply worker's `onBatch`. New pure helper `MapGraphQlNodesToTickets` moved into `GitHubIssueSearchMapping.cpp` (keeps the helper logger-free + doctest-linkable). 5 new bucket-A tests in `GitHubIssueSearchMapping.test.cpp` covering: PR filter on/off, malformed-entry tolerance, non-array input, 4-page accumulation order + isLast-fires-exactly-once.

## Deviations from plan

- **Field source — Strategy B (per-PR enrichment) instead of list-payload extraction**: the plan's § Approach point 3 assumed branch refs + mergeable + draft were available in the `/issues` / `/search/issues` response. They aren't — only `{url, html_url, merged_at}` ride on the `pull_request` sub-object. The shipped implementation issues a per-PR `GET /repos/{o}/{r}/pulls/{n}` after the main list fetch, capped at the same 1000-item ceiling (10×100). Cost: N extra round-trips per refresh when `type:pr` is active. Mitigation: only PR rows enrich; cap matches list cap; missing / null fields tolerated.
- **`includePullRequests` threaded as a separate flag, not encoded in `effectiveQuery`**: the plan suggested signalling "user asked for PRs" via the literal `is:pr` substring in the translated query. Repo-scoped path doesn't use `effectiveQuery` at all (it hits `/repos/.../issues` which doesn't take `q=`), so the substring would never be visible there. The shipped `GitHubFetchPlan.includePullRequests` flag works on both paths uniformly. Cross-repo path additionally injects `is:pr` into the body for server-side filtering.
- **`mergeable == null` encoded as the literal string `"computing"`**: GitHub returns `null` when its merge-check job hasn't finished yet. The plan didn't enumerate this case. The shipped encoding ("computing" vs "true" vs "false" vs "") gives the user a clear next-poll signal.
- **JQL tokenizer accepts `:` as an Op character**: required by `type:pr` shorthand. Side effect — other JQL field handlers (`assignee:foo`) now emit an explicit "Unsupported operator ':'" warning where they previously silently dropped the colon. Slight UX improvement; not a regression.
- **Internal sentinel field `_smatchet_is_pr` in `CachedTicket::fieldValues`**: short-lived marker placed during mapping so the fetch loop knows which rows to enrich, then erased before returning. Not part of any public API. Tests document the contract via the `kIsPullRequestSentinel` constant in `GitHubIssueSearchMapping.h`.
- **Per-page streaming apply was not in v1 of the plan** — the original plan accumulated all pages then emitted a single batch via `ITrackerClient::FetchIssuesStreamed`'s default fallback. Real-world dogfood revealed a ~6s "grid stays empty" perception window even though the UI thread is not blocked (4 serial GraphQL POSTs at ~1.5s each). Slice 1 adds a `onPage` callback to `FetchIssuesViaRestApi` and overrides `GitHubClient::FetchIssuesStreamed` to emit each mapped page immediately. Slice 2 ("cache-first refresh") was investigated and found to be already in place — `AppController::RefreshLocalData()` rehydrates `ActiveTickets` from `LocalCacheManager::GetAllTickets()` at startup, and `TickStreamingApply`'s per-batch merge-by-id (existing) preserves rows across a warm refresh. The intentional `ActiveTickets.clear()` on backend-kind change (Jira ⇄ Plane ⇄ GitHub) was left alone — clearing across kinds is correct (preventing cross-tracker contamination), and the empty-fetch guard already prevents the first-fetch-glitch from wiping cache rows.
- **Strategy C — single GraphQL query per page replaces REST N+1 (post-ship perf fix)**: the original PR12c shipped Strategy B (paginated REST `/search/issues` + per-PR `GET /pulls/{n}`). Real-world dogfood revealed up to 1000 sync HTTP calls per refresh on the worker thread, manifesting as slow load + collateral UI framerate drops. Strategy C ships one `POST /graphql` per page with inline `... on PullRequest { headRefName baseRefName mergeable isDraft mergedAt }` fragments — PR-only fields arrive in the same response, no second fetch. N+1 → N/100. Page cap unchanged (10 pages × 100 = 1000 items, matches REST `/search/issues` hard cap). Repo-scoped JQL is now honoured (GraphQL `search()` accepts qualifier syntax that REST `/repos/{o}/{r}/issues` couldn't), so the prior "repo-scoped fetch ignores JQL filters" warning is gone. GHE endpoint resolution: `<base>/api/v3` → `<base>/api/graphql` (GHE places GraphQL at `/api/graphql`, not `/api/v3/graphql`). Two pure-logic adapters (`MapGraphQlNodeToRestShape`, `MapGraphQlPullRequestNodeToRestPrShape`) keep the existing CachedTicket mapper + PR-enrich helper unchanged. The `kIsPullRequestSentinel` sentinel is still used for transient "is this row a PR" routing within the fetch loop.

## Verification (actual)

- **Bucket A — pure-logic ctest**: `cmake --build --preset ninja-test-msvc` then `ctest --test-dir build/ninja-test-msvc --output-on-failure` — passes (44 GitHub-* doctest cases pass total, including 7 new JQL translator, 3 new fetch plan, 9 new mapping/enrichment).
- **Dual-target build**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — both link clean.
- **Lint**: `clang-format -i` applied to all 8 edited source + 3 edited test files. Build is warning-clean on the touched files.
- **Bucket E (ImGui Test Engine)**: deferred to umbrella PR10 (bucket-E for tracker switch) per the parent plan; no new UI shape change beyond column population (the 4 `pr.*` columns render via the existing field-catalog rendering path).
- **Manual residue**: setting view JQL to `type:pr` and observing PR rows in the running exe — to be exercised post-merge by the user; no test-author backlog entry needed because the verifiable surface (translator → plan → mapping → enrichment) is fully bucket-A covered.

## Sequencing

Stacked on PR4 (`#387`). Lands after `#387` merges to develop. Estimated ~300 LOC + ~120 LOC tests.
