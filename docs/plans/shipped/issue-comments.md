# Plan — Issue comments (all 3 backends): count column + read/post modal

> **Slug**: `issue-comments` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — all cited PRs merged (see Implementation log); archived 2026-06-16 via plan-archival sweep.
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Surface more issue data inside the tracker grid (origin: issue #1291). Today no backend
surfaces a comment thread in the UI. Goal: a grid column whose cell click opens a modal that
**reads the full comment thread + lets the user post a new comment**, for **all three backends**
(GitHub, Jira, Plane). After this lands: from any tracker grid the user can see comment counts
(GitHub/Jira) or a comment affordance (Plane), open a thread, and reply — without leaving Smatchet.

The `ITrackerCollaboration` interface (`FetchIssueComments` + `AddIssueCommentPlain`) already
exists with backend-agnostic `TrackerIssueComment`; both methods default to "not supported." This
plan implements the per-backend overrides + one new UI surface, shipped as 3 phased PRs.

## Approach

Write the UI **once**, backend-agnostic: a comments column cell + a deferred-open read/post modal
+ an `AppController::FetchIssueComments` off-UI read wrapper (the post wrapper already exists).
"Enabling comments" for a backend then = implementing its two `ITrackerCollaboration` overrides.
Ship in 3 PRs so the shared UI lands and is proven once (GitHub), then each backend's overrides
follow:

- **PR-A** — shared UI + modal + AppController read wrapper + **GitHub** end-to-end (both overrides,
  catalog field, free count from the search payload).
- **PR-B** — **Jira** read override (post already shipped) + free count from the comment `total`.
- **PR-C** — **Plane** read + post from scratch (UUID resolution, `comment_html` body) + an
  **icon-only** grid cell (no count — a per-row count fetch would violate Pillar 2).

Trade-off: Plane has no comment count in its list payload, so rather than a per-row network fetch
(Pillar-2 violation) its cell shows a bare 💬 affordance; the count is implicit until the modal opens.

## Files to modify

**PR-A — shared UI + GitHub** (owners: `tracker-backend`, `grid-engine`, `test-rig`)
1. `Source/Core/include/Tracker/GitHubClient.h` — declare `FetchIssueComments` + `AddIssueCommentPlain` overrides.
2. `Source/Core/src/Tracker/GitHubClient.cpp` — both override bodies (via `TrackerHttpClient`, `.../issues/{N}/comments`); `addField("github.comments","Comments","number")`; `FetchIssueEditMeta` non-editable flag.
3. `Source/Core/src/Tracker/GitHubCommentMappingPure.{h,cpp}` (new) — pure JSON→`TrackerIssueComment` map (unit-testable).
4. `Source/Core/src/Tracker/GitHubIssueSearchMapping.cpp:~180` — set `fieldValues["github.comments"]` via a new int extractor (mirrors `IssueNumberString`; no `JsonInt` helper exists).
5. `Source/Core/include/AppController.h` + `Source/Core/src/AppController_CatalogAndFieldEdit.cpp:1727` — `FetchIssueComments` off-UI read wrapper (no read-only guard) mirroring `AddIssueCommentPlain`.
6. `Source/Core/include/Ui/SmatchetUiSession.h` — `CommentsModalState` struct.
7. `Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp:~1318,~1384,~549` — comments cell special-case (count + icon-only branches), gesture → `OpenCommentsModal`, `RenderCommentsModal` beside `RenderLongTextModal` (+ new comments-modal source file if it grows past inline).
8. `Source/Core/src/SmatchetLocalization.cpp` — EN+FR strings (Comments / Post comment / Loading comments… / hint / read-only note).
9. `tests/Core/GitHubCommentMappingPure.test.cpp` (new) + `tests/Core/GitHubIssueSearchMapping.test.cpp` — pure map + count-mapping regression.

**PR-B — Jira read** (owner: `tracker-backend`, `test-rig`)
10. `Source/Core/include/Tracker/JiraClient.h` — declare `FetchIssueComments` override.
11. `Source/Core/src/Tracker/JiraIssueSearch.cpp` — `FetchIssueComments` body: base+headers from cfg → existing `JiraFetchIssueCommentsPages` → pure map.
12. `Source/Core/src/Tracker/JiraIssueMappingPure.cpp:177` (`ResolveJiraCommentField`) — set `fieldValues["comments"] = std::to_string(totalComments)`; ensure `comments` catalog field + read-only.
13. `Source/Core/src/Tracker/JiraCommentMappingPure.{h,cpp}` (new) + test — pure raw-Jira-comment→`TrackerIssueComment` map reusing `ParseCommentAuthor` / ADF→plain.

**PR-C — Plane read+post + icon-only cell** (owners: `tracker-backend`, `grid-engine`, `test-rig`)
14. `Source/Core/include/Tracker/PlaneClient.h` — declare both overrides.
15. `Source/Core/src/Tracker/PlaneIssueMutation.cpp` — both bodies: resolve slug/project/UUID under `planeCacheMutex_` → `.../work-items/{uuid}/comments/` GET/POST; `comment_html` via `MarkdownConvert`.
16. `Source/Core/src/Tracker/PlaneCommentMappingPure.{h,cpp}` (new) + test — JSON→`TrackerIssueComment` (`Body` from `comment_stripped`).
17. `Source/Core/src/Tracker/PlaneFieldCatalog*.cpp` — Plane comments catalog field with no count value (icon-only branch in the existing cell special-case).

## Existing utilities reused

- `ITrackerCollaboration::FetchIssueComments` / `AddIssueCommentPlain` + `TrackerIssueComment` — `Source/Core/include/ITrackerCollaboration.h` — the backend-agnostic contract (Body is plain text; UI renders no rich formatting).
- `AppController::AddIssueCommentPlain` — `Source/Core/src/AppController_CatalogAndFieldEdit.cpp:1727` — off-UI post wrapper + read-only guard; the read wrapper mirrors it.
- `app.LaunchBackgroundTask(...)` → `app.mainThreadDispatcher.PostToMainThread(...)` + toasts — `Source/Core/src/Ui/SmatchetGridUiSupport.cpp:245` — worker pattern for all fetch/post.
- Deferred-open modal mechanism — `RenderLongTextModal` @ `SmatchetActiveProjectGridUi.cpp:549`, in-cell-OpenPopup-illegal note @ `:593` — mirror the mechanism (not `OpenLongTextEditor`, which is field-tied to `TrackerField&`).
- `ParseGitHubIssueKey` (`GitHubClientHelpers.cpp:16`) + `BuildIssuePatchUrlSuffix` (`:162`) — GitHub key → `/repos/{o}/{r}/issues/{n}`; `+/comments`.
- `JiraFetchIssueCommentsPages` (`JiraIssueSearch.cpp:18`) + `NormalizeBaseUrl` + `BuildTrackerHeaders` (`:315`) — Jira raw-comment fetch; `ParseCommentAuthor` / `ExtractAdfTextToStream` / `CollectAdfText` / `FormatDateIfIso` from `TrackerFieldValueParser.cpp:415` (`ParseComments`) — ADF→plain per-node helpers.
- `ResolvePlaneProject` / `LooksLikeUuid` / `planeCacheMutex_` (`PlaneIssueMutation.cpp:25,62`) + `MarkdownConvert.h` — Plane UUID resolution + md↔html.
- `SetActiveIssue()` / `pane.gridState.ActiveIssueId` — `SpreadsheetState.h:85,69`.
- `GetAvailableFields()` (`AppController.h:608`) — a new catalog field auto-appears in Views → Fields tab (no picker code).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: cell render is a string draw (count) or a glyph (icon) — no per-frame work. Count is mapped once at search time (GitHub/Jira) into `fieldValues`. No impact.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: every fetch + post runs on `LaunchBackgroundTask` → `PostToMainThread`; modal shows an in-flight spinner; **no network on hover and no per-row network** (Plane is icon-only precisely to avoid a per-row count fetch).
- **Pillar 3 (never crash)**: RAII throughout; pure mappers tolerate missing/null JSON fields (default empty/0); modal state is value types; empty `catch(...)` banned.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: modal uses standard ImGui widgets (inherit keyboard nav + font scaling); 💬 glyph paired with a text tooltip / count so it is not icon-only-for-meaning where a count exists. No regression.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

1. **PR-fast CI** — scenario most directly exercising the path: the grid-render / tracker-grid scenario (cell special-case is in `SmatchetActiveProjectGridUi`). Modal open is user-gesture-driven, not in the steady-state scenario. Run the grid scenario pre-push.
2. **Pillar 2 static scanner** — **no new sync-I/O reachable from `ImGui::*`**: all fetch/post go through `LaunchBackgroundTask`. No `/* PILLAR2_WORKER_ONLY */` annotation needed (no sync I/O on the UI thread at all).
3. **Dispatcher drain** — does not touch `MainThreadDispatcher::Drain()`; uses the existing `PostToMainThread` enqueue path only.
4. **Visible-cue bucket-E harness** — adds no new sync-stall code path > 100 ms (all stalls are off-thread with a spinner cue). N/A for a new bucket-E stall case.
5. **Marker inventory** — adds no `SMATCHET_UI_PERF_SCOPE` markers (will add a temporary one only if a hitch is observed during manual verify; if any ship, regen `docs/perf/MARKER_INVENTORY.md` same PR). Currently N/A.

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline against the grid scenario before opening each PR.

**Override**: none expected (no intentional regression).

## Risks / non-goals

- **Plane comment API untested against a live instance** — mitigation: pure mapper is unit-tested against captured-shape fixtures; live verify in PR-C manual step; the endpoint shape mirrors the existing `work-items/{uuid}/` PATCH path already in `PlaneIssueMutation.cpp`.
- **GitHub/Jira comment pagination** — both fetch loops page to completion (`per_page=100` / Jira pages); risk: very long threads. Mitigation: cap the rendered thread (mirror `ParseComments` kMaxComments=20 style) with a "showing N of M" note. Accepted for v1.
- **Comments bypass offline-queue + audit trail** — accepted: the queue/audit invariant is scoped to issue creates + field edits (precedent); comments are direct-post, same as the existing Jira post.
- **Non-goal**: editing / deleting existing comments — read + post-new only. **Non-goal**: rich-text rendering of comment bodies — plain text per the `TrackerIssueComment.Body` contract. **Non-goal**: @-mention autocomplete. **Non-goal**: Plane comment count column (icon-only by design).

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `GitHubCommentMappingPure` (login/body/epoch parse, missing fields, empty array), `GitHubIssueSearchMapping` count-maps from `issue["comments"]` (absent/zero on commit rows), `JiraCommentMappingPure` (ADF→plain, author, date), `PlaneCommentMappingPure` (`comment_stripped`→Body). `ninja-test-msvc` → `SmatchetTests.exe` green.
- **Bucket E (ImGui Test Engine)**: N/A for PR-A/B (modal is gesture-driven; covered by manual + the deferred bucket-E backlog item below). If feasible, a bucket-E case opening the modal and asserting the spinner→thread transition is added in PR-A; else deferred with a tooling entry.
- **Bash-driver scenario / screenshot / sanitizer**: ASAN build clean (Sanitizer job); grid scenario screenshot diff for the new column glyph.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) per PR.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run before finalising; record the outcome below.
- **Manual residue**: modal open/post flow is manually verified per PR (GitHub on #1291, Jira on a live instance, Plane on a live instance). If a bucket-E case proves infeasible, a `docs/self-improvement/categories/tooling.md` entry tracks the deferred automation. No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Comment edit / delete — follow-up plan if requested.
- Plane comment count column — no-action (icon-only by design; a count needs a per-row fetch = Pillar-2 violation).
- Rich-text / markdown rendering of comment bodies — no-action (plain-text Body contract).
- bucket-E automation of the modal flow if not landed in PR-A — `docs/self-improvement/categories/tooling.md` follow-up.

## Implementation log
- `3c10e1d1` · PR-A #1217 — shared comments UI/modal + AppController read wrapper + GitHub end-to-end (both `ITrackerCollaboration` overrides, catalog field, free count from search payload).
- `1aa25cf1` · PR-B #1218 — Jira `FetchIssueComments` read override + free count from the comment `total`.
- `2e0940ad` · PR-C #1219 — Plane read+post from scratch (UUID resolution, `comment_html` body) + icon-only grid column (no count).
- `1cce7c12` · consolidating fix #1266 — wired `SmatchetCommentsModalUi` into the active-project grid and reconciled the 3 backends.

## Deviations from plan
- Explicitly deferred (no-action / follow-up): comment edit/delete, the Plane comment count column (icon-only by design — a per-row count fetch would violate Pillar 2), rich-text/markdown rendering of comment bodies (plain-text `Body` contract), and bucket-E modal-flow automation.

## Verification (actual)
- Deliverables verified present in tree (archival audit 2026-06-16): GitHub/Jira/Plane `CommentMappingPure` modules + tests; `FetchIssueComments` overrides in all three clients; `SmatchetCommentsModalUi.{h,cpp}` wired into `SmatchetActiveProjectGridUi.cpp`.
- Test/build gates (Bucket A ctest, dual-target build, ASAN, doc-validation, grid-scenario perf): verified present in tree (archival audit 2026-06-16), not re-run.
