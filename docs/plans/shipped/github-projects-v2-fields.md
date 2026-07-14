# Plan — GitHub Projects v2 custom fields + ListProjects

> Closes the two items `docs/plans/shipped/github-tracker-backend.md` left deferred after
> its § Remaining list shipped: `GitHubClient::ListProjects` (empty since PR2) and
> Projects v2 custom fields via GraphQL (deferred since the original plan's § Out of scope).

## Context

GitHub issues have no configurable field schema — the tracker backend ships a static
catalog of native columns (title/body/state/assignees/labels/milestone + PR/commit
projections). Real GitHub planning workflows keep their custom metadata (priority,
estimate, sprint, due date, team notes) on a **Projects v2 board**, reachable only via
GraphQL. Without those fields Smatchet's grid can't show or edit the columns users
actually plan with.

Separately, `GitHubClient::ListProjects()` returned `{}` since PR2, so the project
picker offered nothing for GitHub even though the create path (`BuildCreatePayload`)
already keys on an `owner/repo` `ProjectKey`.

## Decisions locked

1. **Config anchor = owner + project number** — one board per config
   (`github_project_number` int, 0 = off, under the existing `github_owner`). The N in
   `github.com/orgs/<owner>/projects/N`. No board auto-discovery in v1.
2. **Field ids are lowercase slugs `pv2.<name-slug>`** — `EditMetaCacheService`
   lower-cases lookup keys AND is default-deny for ids missing from a loaded map, so a
   mixed-case GraphQL node id could never round-trip. The case-SENSITIVE field node id
   travels in `TrackerField::SchemaCustom` (and inside the BuildFieldPayload output),
   never as the catalog id. `FetchIssueEditMeta` whitelists every `pv2.*` id.
3. **Typed-value conversion happens in `BuildFieldPayload`** — the only mutation-path
   stop that still holds the catalog `TrackerField` (options list + node id). The
   payload embeds `{"fieldId": <node id>, "value": <ProjectV2FieldValue|null>}`;
   `BuildGitHubIssueUpdatePlan` collects `pv2.*` entries into `ProjectV2Edits`;
   `UpdateIssueFields` executes them via `updateProjectV2ItemFieldValue` /
   `clearProjectV2ItemFieldValue` (null = clear). A bare-string pv2 payload (offline
   queue's verbatim fallback, which cannot carry the node id) is rejected with a
   pointer back at the grid.
4. **Representable families only** — TEXT, NUMBER (validated), DATE (strict
   YYYY-MM-DD), SINGLE_SELECT (option id resolved from display name or id), ITERATION
   read-only (no iteration-id picker in v1). Built-in projections (TITLE/ASSIGNEES/…)
   are skipped — the native columns already cover them.
5. **Read paths degrade, the write path surfaces** — a board fetch failure appends a
   catalog/sync `Warning` and the native fields still load; a pv2 edit failure is a
   real `TrackerError`. Smatchet never auto-adds issues to a board: editing a field of
   an issue that is not a board item errors with "add it to the project on GitHub
   first".
6. **One catalog fetch per client** — `EnsureProjectV2Catalog` caches under its own
   mutex keyed on owner+number (failures are NOT cached); per-sync item values are one
   paginated walk (100/page, 5-page cap with truncation warning) joined onto tickets
   by `owner/repo#N`.
7. **ListProjects = repositories** — paginated `GET /user/repos` (100/page, 10-page
   cap, short-page break), each repo one `RemoteProject{id=repo id, key=full_name,
   displayName=full_name}`; `key` feeds `IssueDraft::ProjectKey`'s `owner/repo` split.
   Best-effort: any failure returns `{}` (PlaneClient precedent).

## Files

- **NEW** `Source/Core/include/Tracker/GitHubProjectsV2Pure.h` + `src/Tracker/GitHubProjectsV2Pure.cpp`
  — pure seam (cpr-free): the five GraphQL documents, `ParseProjectV2Catalog`,
  `ParseProjectV2ItemsPage`, `ParseProjectV2ItemIdForProject`,
  `BuildProjectV2FieldValue`, `SlugifyProjectV2FieldName`.
- **NEW** `Source/Core/src/Tracker/GitHubProjectsV2.h` + `.cpp` — cpr-bound shell:
  `RunProjectV2Query` (POST + bounded parse + HTTP/errors[] classification;
  `tolerateErrors` for the dual org/user catalog query), `FetchProjectV2Catalog`,
  `FetchProjectV2ItemValues`, `ResolveProjectV2ItemId`, `UpdateProjectV2FieldValue`.
- `GitHubIssueSearch.{h,cpp}` — `ResolveGraphQlEndpoint` + `ExtractGraphQlErrors`
  hoisted out of the anon namespace (shared with the projects-v2 shell).
- `GitHubMutationPure.{h,cpp}` — `GitHubProjectV2Edit` + `ProjectV2Edits` on the plan;
  `BuildGitHubIssueUpdatePlan` handles `pv2.*` ids per decision 3.
- `GitHubClient.{h,cpp}` — catalog cache members + `EnsureProjectV2Catalog` +
  `FetchProjectV2ValuesForSync`; enrichment merge in `FetchIssues` /
  `FetchIssuesStreamed`; `FetchFieldCatalog` append + Warning degrade;
  `FetchIssueEditMeta` pv2 whitelist; `BuildFieldPayload` pv2 branch;
  `UpdateIssueFields` pv2 execution (`ApplyGitHubProjectV2Edits`, `pv2_field_update`
  audit rows under the outer op); `ListProjects` REST walk.
- `GitHubIssueSearchMapping.{h,cpp}` — `AppendGitHubReposAsRemoteProjects`.
- `Config/ConfigManager.{h,cpp}` — `GitHubProjectNumber` int field (registry-table read/write).
- `Ui/SmatchetUiSession.h` + `Ui/SmatchetPreferencesUi.cpp` — "Project number" InputInt
  in the GitHub tracker section (load/save/dirty-check wired like the owner/repo buffers).
- Tests: **NEW** `tests/Core/GitHubProjectsV2Pure.test.cpp`, **NEW**
  `tests/Core/GitHubProjectsV2Http.test.cpp` (loopback fixture routes the single
  `/graphql` path by request-body content), pv2 cases in `GitHubMutationPure.test.cpp`,
  repo-mapper cases in `GitHubIssueSearchMapping.test.cpp`, ListProjects fixture cases
  in `GitHubClientHttp.test.cpp` (under `TestEnvGuard` — cfg-less path).

## Pillars

- **Pillar 2 (no UI-thread I/O)**: catalog + item-value fetches run on the sync worker /
  mutation paths; the Preferences input is a plain int buffer. `RunProjectV2Query`
  carries the worker-only latency marker.
- **Pillar 3 (null/missing-safe ingress)**: every GraphQL response goes through
  `ParseBoundedOrDiscarded`; parsers tolerate missing/null nodes (draft items, absent
  repos, unknown field node ids) by skipping, never throwing.

## Verification (actual)

- `SmatchetTests` (Linux rig, `build/linux-tests`, run from repo root): 2504/2505
  passed — the 1 failure is the known pre-existing `SubprocessCapturePure` UTF-16
  wchar_t case (Windows-only semantics).
- New coverage: 15 test cases / 186 assertions across the pure seam (documents pinned,
  catalog/items/item-id parsing, value building, slugify), the GraphQL shell
  (dual-owner tolerance, pagination, error classification), the GitHubClient
  integration (catalog append + Warning degrade + editmeta whitelist, UpdateField pv2
  end-to-end wire shapes incl. clear + not-on-board + option-mismatch + errors[]),
  the update-plan builder, the repo mapper, and ListProjects.
- Lint gate `agents/scripts/project/test-lint-rules.sh --diff origin/develop`: clean.

## Out of scope (flagged, not designed)

- Iteration-field WRITES (needs an iteration-id picker; read-only column ships now).
- Multi-board support / board auto-discovery.
- Auto-adding issues to a board on first edit (`addProjectV2ItemById`).
- Draft project items (no issue/PR content to join onto a grid row).

## Implementation log

- 2026-07-14 — full slice implemented + tested on `claude/github-tracker-backend-docs-3194ke`
  (single PR: pure seam, GraphQL shell, GitHubClient wiring, config + Preferences UI,
  ListProjects, tests, docs).
