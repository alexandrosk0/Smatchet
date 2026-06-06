# Plan — User Info Window

> **Slug**: `user-info-window` (matches this file's basename without `.md`).
>
> **Status**: `active` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section. Sections that genuinely don't apply get `N/A — <one-line reason>`, not deletion.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Implements the clean-room **User Info Window** spec: a full-window inspector for a single person — identity, recent Perforce submissions, recent tracker activity, and the team groups they belong to. Opened by selecting a user elsewhere in the app; acts as a launch point into p4v, the tracker (browser), and the app's active query.

The spec is tracker-agnostic. Smatchet's active backend is pluggable (`ITrackerBackend` → Jira / Plane / GitHub). Today **only `JiraClient` implements `ITrackerCollaboration`** (group-name + user search); there is **no activity-feed struct/endpoint**, **no group-members fetch**, **no global group catalog**, and **no p4-changes-by-user** query. This plan adds that backend surface (Jira + Plane), the P4 query, two config keys, and the window itself.

**After this lands**: selecting a user opens a full window showing their last-N p4 submissions, a day-windowed tracker activity feed (assignee/reporter history), and expandable team-group membership, with click-to-p4v / add-to-query / copy interactions throughout — working on both Jira and Plane backends (GitHub degrades to empty).

Scope decisions (confirmed with user 2026-06-06, refined in the `grill-with-docs` pass):
- **Tracker support = Jira full + Plane/GitHub graceful-degrade.** `PlaneClient::Collaboration()` returns `nullptr` ([PlaneClient.cpp:149](../../../Source/Core/src/Tracker/PlaneClient.cpp:149)) — Plane exposes no collaboration role at all. So "Jira + Plane" means the window **null-checks `Collaboration()`** and renders empty tracker sections for Plane/GitHub (identity + P4 still work) via the existing `AppController` null-guard path. **Zero new Plane/GitHub backend code** (enabling Plane collaboration is a separate future plan).
- **Activity feed = one row per Jira changelog item authored by the user.** Discovery: JQL `assignee WAS "{user}" OR reporter WAS "{user}"` over `updated >= -Ddays` → per-issue `expand=changelog` → keep history items whose author = the user and whose `created` falls in the window. Progress counter = issues-scanned / total.
- **Issue-key detection = new `FindFirstIssueKeyInText` helper in `ProjectResolver`, delegating per-token to the existing `ExtractIssueKeyPrefix`.** Reuse keeps key-validation single-source (your choice); only a word-boundary tokenizer is new. `ExtractIssueKeyPrefix` alone can't do embedded detection — it's an anchored full-string validator.
- **`onAddToQuery` = replace active-view JQL with `key = "{key}"`** via the existing `cfg.JqlQuery` + `ViewState.UpdateActive` + `ConfigManager::Save` path. **Deliberate deviation from spec 7.4**: the activity Issue cell passes the parsed **key** (not the issue URL) to add-to-query, so the query actually works; the URL is kept only for the context-menu "Open in Browser". (Spec flags the URL behavior as a bug to fix-deliberately-not-by-accident — we fix it.)
- **Shared P4 CL-preview tooltip + p4v launcher = extracted to public helpers** (`DrawClTooltipAsync` / `LaunchP4VcLike` are today coupled to `AnnotateAnalysisUi`'s private `_Internal.h` + a single global hover slot). Extraction moves the hover state out of the Annotate singleton (key it by CL) so two windows don't collide.
- **All activity + group state is in-memory.** No SQLite table, no schema bump, no migration. Only persisted addition = the JSON config keys. Terms canonicalised in [Tracker `CONTEXT.md`](../../../Source/Core/src/Tracker/CONTEXT.md): `TrackerActivityEntry`, **"group roster"** (`GroupMemberCache`) — NOT "group catalog" (collides with **Field catalog**).
- Delivered as **3 sliced PRs**.

## Approach

**Three slices, each its own PR** (per § PR-batching — split along seams; the full diff would exceed the CodeRabbit per-PR file ceiling):

**Slice 1 — P4 foundation + shared-helper extraction + config keys (no UI window).**
(a) Add `P4ChangesForUser(cfg, p4user, maxN) -> vector<P4ChangeSummary>` to the P4 layer (new `p4 changes -u <user> -m N -s submitted` invocation, reusing `P4RunCommand`); pure-logic output parse is bucket-A testable. (b) **Extract** the shared CL-preview tooltip + p4v launcher out of `AnnotateAnalysisUi_Internal.h` into public helpers (`P4ClPreview.{h,cpp}` + `P4vLaunch.{h,cpp}`), moving the hover slot off the Annotate singleton (key by CL); repoint `AnnotateAnalysisUi` to the extracted helpers (DRY-positive, no behaviour change). (c) Add config keys via the existing table-driven `FieldDesc`/backend-block pattern (clamp like `cl_cache_max`): `ProductionGroupKeyword` (string), `UserActivityDayWindow` (int, default 30), `MaxUserChanges` (int).

**Slice 2 — tracker backend: activity feed + group members (Jira only; strict zone).**
Extend `ITrackerCollaboration` (backend-agnostic — no `Jira*` leak) with: `FetchUserActivity(accountId, dayFrom, dayTo, progress&, out&, err&)` returning the new in-memory `TrackerActivityEntry` (timestamp, issueKey, issueUrl, summary, actionLabel, details); `FetchGroupMembers(groupName, out&, err&)`; `ClearUserActivity()`; plus a `TrackerActivityProgress` (current/total) for the loading bar. **Jira implements** (one changelog-item-per-row, author-filtered, JQL `assignee WAS / reporter WAS` discovery — the Jira-shaped JQL stays behind `JiraClient`, e.g. a `JiraActivityFeed.{h,cpp}`); **Plane/GitHub keep returning `nullptr` Collaboration** → callers degrade empty. The in-memory **group roster** (group → members, `GroupMemberCache`) is held in `AppController` / the collaboration layer, loaded lazily. All HTTP routes through `TrackerHttpClient` (subsystem invariant). Delegate to `tracker-backend`.

**Slice 3 — the UI window.**
New `SmatchetUserInfoUi` (header + P4 section + activity section + groups section + shared issue-key context-menu popup), following the `AnnotateAnalysisUi` / `SmatchetPerfUi` full-window pattern: `DrawWindow(AppController&, UiDrawSession&, bool* pOpen)` + `Open(displayName, email, accountId)`, registered in `SmatchetUI::drawSecondaryWindows()`. Reuse the **extracted** `P4ClPreview` tooltip + `P4vLaunch`. `onStatus` / `onAddToQuery` thread through `UiDrawSession`; `onAddToQuery` host impl replaces active-view JQL with `key = "{key}"` via the existing view-mutation path. Issue/details/description key rendering uses `FindFirstIssueKeyInText`. All backend/P4 fetches run on a **worker thread**, posting back via `MainThreadDispatcher` (the `SmatchetAuditUi` pattern) — never sync on the ImGui thread (Pillar 2). Window null-checks `Collaboration()` → empty tracker sections on Plane/GitHub. One-shot group-fetch flag; Escape/Close → `ClearUserActivity`; per-render group resolution. Delegate to `grid-engine` / general UI.

Trade-off: the activity feed is the riskiest surface (self-designed JQL+changelog contract, potentially many changelog fetches for active users). It is isolated in Slice 2 so the UI (Slice 3) builds against a populated-or-empty feed without blocking on backend tuning; day-windowing + author-filter bound the row/fetch volume.

## Files to modify

> Grepped: no existing `UserInfo*` / `SmatchetUserInfoUi` TU (investigation confirmed 0 defs). `TrackerActivityEntry`, `P4ChangesForUser`, `FetchUserActivity`, `FetchGroupMembers`, `FindFirstIssueKeyInText`, `P4ClPreview`, `P4vLaunch` do not exist under any synonym.

**Slice 1 — P4 foundation + shared-helper extraction + config**
1. [P4Annotate.h](../../../Source/Core/include/P4Annotate.h:30) — add `P4ChangeSummary` struct (CL number, date, user, first-line description) + `P4ChangesForUser` decl.
2. [P4Annotate.cpp](../../../Source/Core/src/P4Annotate.cpp:112) — implement `P4ChangesForUser` via `p4 changes -u <user> -m N -s submitted`, reusing `P4RunCommand`; parse into `P4ChangeSummary`.
3. New `Source/Core/include/Ui/P4ClPreview.{h}` + `Source/Core/src/Ui/P4ClPreview.cpp` — **extract** `DrawClTooltipAsync` out of `AnnotateAnalysisUi_Internal.h`/`_Modals.cpp`; hover state keyed by CL (off the Annotate singleton).
4. New `Source/Core/include/Ui/P4vLaunch.{h}` + `Source/Core/src/Ui/P4vLaunch.cpp` — **extract** `LaunchP4VcLike` out of `AnnotateAnalysisUi_Launch.cpp` to a public helper.
5. [AnnotateAnalysisUi_Modals.cpp:432](../../../Source/Core/src/Ui/AnnotateAnalysisUi_Modals.cpp:432), [AnnotateAnalysisUi_Launch.cpp:122](../../../Source/Core/src/Ui/AnnotateAnalysisUi_Launch.cpp:122), [AnnotateAnalysisUi_Window.cpp:396](../../../Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp:396) — repoint Annotate to the extracted helpers (no behaviour change; DRY-positive).
6. [ConfigManager.h](../../../Source/Core/include/Config/ConfigManager.h:509) — add `ProductionGroupKeyword` (string), `UserActivityDayWindow` (int), `MaxUserChanges` (int) fields.
7. [ConfigManager.cpp](../../../Source/Core/src/Config/ConfigManager.cpp:743) — register the new keys via the table-driven `FieldDesc` rows / backend-block (clamp like `cl_cache_max`, [ConfigManager.cpp:589](../../../Source/Core/src/Config/ConfigManager.cpp:589)).

**Slice 2 — tracker backend (strict zone `Source/Core/src/Tracker/`; Jira only)**
8. [ITrackerCollaboration.h](../../../Source/Core/include/ITrackerCollaboration.h:62) — add backend-agnostic `TrackerActivityEntry` struct, `TrackerActivityProgress` (current/total), and virtual methods `FetchUserActivity`, `FetchGroupMembers`, `ClearUserActivity`. **No `Jira*` leak** into the interface (subsystem invariant).
9. [JiraClient.h](../../../Source/Core/include/Tracker/JiraClient.h:146) — declare the three new overrides.
10. [JiraUserAndMeta.cpp](../../../Source/Core/src/Tracker/JiraUserAndMeta.cpp:366) — implement `FetchGroupMembers` (GET group members, via `TrackerHttpClient`).
11. New `Source/Core/src/Tracker/JiraActivityFeed.{h,cpp}` — JQL `assignee WAS / reporter WAS` window discovery + `expand=changelog` parse → one `TrackerActivityEntry` per user-authored changelog item in-window + progress counter. HTTP via `TrackerHttpClient`.
12. [AppController.h](../../../Source/Core/include/AppController.h:948) + [AppController_CatalogAndFieldEdit.cpp:1609](../../../Source/Core/src/AppController_CatalogAndFieldEdit.cpp:1609) — delegator methods (null-check `Collaboration()` like `FetchUserGroupNames` does) + lazy in-memory **group roster** (`GroupMemberCache`, group → members). **No PlaneClient/GitHubClient edits** — they keep returning `nullptr` Collaboration → degrade empty.
13. [ProjectResolver.h](../../../Source/Core/include/Tracker/ProjectResolver.h:9) + [ProjectResolver.cpp:21](../../../Source/Core/src/Tracker/ProjectResolver.cpp:21) — add `FindFirstIssueKeyInText(text) -> {key,start,end}` that tokenizes on word boundaries and validates each candidate via the existing `ExtractIssueKeyPrefix` (single-source validation).

**Slice 3 — UI**
14. New `Source/Core/include/Ui/SmatchetUserInfoUi.h` + `Source/Core/src/Ui/SmatchetUserInfoUi.cpp` — the window + `Open(displayName,email,accountId)` (light zone; ImGui-draw 200-line cap; split into `_Sections.cpp` if it grows). Worker-thread fetch + `MainThreadDispatcher` post-back.
15. [SmatchetUI.h](../../../Source/Core/include/Ui/SmatchetUI.h:193) — own the `SmatchetUserInfoUi` instance + declare in `drawSecondaryWindows()`.
16. [SmatchetUI.cpp](../../../Source/Core/src/Ui/SmatchetUI.cpp:193) — register render call.
17. [SmatchetUiSession.h](../../../Source/Core/include/Ui/SmatchetUiSession.h:139) — thread `onAddToQuery` (replace active JQL with `key = "{key}"` via the view-mutation path) / `onStatus`.
18. CMake source-list gating — add **all** new TUs (Slices 1 + 3) to both `SmatchetStandalone` and `SmatchetCore_DX12` lists (dual-target).

## Existing utilities reused

- `P4ClPreview` / `P4vLaunch` (extracted in Slice 1 from `DrawClTooltipAsync` [AnnotateAnalysisUi_Modals.cpp:432](../../../Source/Core/src/Ui/AnnotateAnalysisUi_Modals.cpp:432) + `LaunchP4VcLike` [AnnotateAnalysisUi_Launch.cpp:122](../../../Source/Core/src/Ui/AnnotateAnalysisUi_Launch.cpp:122)) — shared CL-preview tooltip + p4v launcher; **reused, not reimplemented** (spec 6.2, 7.5). Both are currently private to `AnnotateAnalysisUi_Internal.h` and the tooltip shares one global hover slot — extraction is the prerequisite for clean reuse.
- `P4RunCommand` — [P4Annotate.cpp:29](../../../Source/Core/src/P4Annotate.cpp:29) — p4 invocation wrapper for the new changes-by-user query.
- `P4ChangelistDescribeCache` — [P4Annotate.h:62](../../../Source/Core/include/P4Annotate.h:62) — LRU describe cache feeding the preview tooltip.
- `ProjectResolver::ExtractIssueKeyPrefix` — [ProjectResolver.cpp:21](../../../Source/Core/src/Tracker/ProjectResolver.cpp:21) — anchored full-string key validator; the new `FindFirstIssueKeyInText` delegates to it per token so validation stays single-source (it already guards UUIDs/digit-leading strings → Plane keys won't false-match, addressing spec §12). **Asymmetry deliberately fixed**: add-to-query receives the parsed **key** everywhere; the issue **URL** is used only for context-menu browser-open (deviates from spec 7.4 by design).
- `AppController::FetchUserGroupNames` null-guard — [AppController_CatalogAndFieldEdit.cpp:1609](../../../Source/Core/src/AppController_CatalogAndFieldEdit.cpp:1609) — the existing `if (!backend->Collaboration())` degrade pattern the new delegators copy.
- `JiraClient::FetchUserGroupNames` / `SearchUsersByQuery` — [JiraUserAndMeta.cpp:316](../../../Source/Core/src/Tracker/JiraUserAndMeta.cpp:316) — existing group-name + user lookup; activity/members build alongside, all through `TrackerHttpClient`.
- `TrackerUser` (AccountId / DisplayName / EmailAddress) — [TrackerFieldSchema.h:60](../../../Source/Core/include/Tracker/TrackerFieldSchema.h:60) — identity fields; p4 username derives from email local-part (spec 6.1, edge-case §11).
- `SmatchetAuditUi` worker-thread → `MainThreadDispatcher` post-back pattern — [SmatchetAuditUi.cpp:84](../../../Source/Core/src/Ui/SmatchetAuditUi.cpp:84) — template for keeping all fetches off the ImGui thread (Pillar 2).
- `AnnotateAnalysisUi` / `SmatchetPerfUi` full-window pattern — [AnnotateAnalysisUi.h:18](../../../Source/Core/include/Ui/AnnotateAnalysisUi.h:18) — `DrawWindow(...)` + `drawSecondaryWindows()` registration template.
- Active-query JQL mutation — [SmatchetActiveProjectGridUi.cpp:532](../../../Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp:532) — the `cfg.JqlQuery` + `ViewState.UpdateActive` + `ConfigManager::Save` path `onAddToQuery` reuses.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: Window draws tables (~250px scroll regions) — bounded by visible rows. Activity visible-window filtering (spec 7.2: string compare on `YYYY-MM-DD`) runs per-frame over a snapshot list; keep the list bounded and the filter O(n) cheap. No per-frame allocation in the draw path.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: All I/O (p4 changes fetch, JQL activity fetch, group-members fetch, p4 describe for tooltips) is **worker-thread / async** — never synchronous in the ImGui draw path. Activity load shows a progress bar (spec 7.6) driven by the current/total counter; p4 fetch logs "Loading…" and the load button disables while in flight. CL tooltip uses the existing async `DrawClTooltipAsync`. Any new sync-I/O reachable from `ImGui::*` is a CRITICAL — must carry `/* PILLAR2_WORKER_ONLY */`.
- **Pillar 3 (never crash)**: RAII throughout; bounds-check all parsing (p4 changes output, changelog JSON, 6+-digit CL scan, estimate-seconds parse → leave unchanged on failure per spec 7.5.1). Graceful degrade for Plane/GitHub (empty groups/activity, never throw). Empty `catch(...)` banned.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: Escape closes (spec 3.3) — keyboard-reachable. Color roles map to Smatchet's config palette (contrast inherited from theme). Full keyboard nav of tables is aspirational/backlogged (consistent with existing windows); no new auto-fail.

## Perf-review-system gates (mandatory — diff touches `Source/Core/`)

Per `docs/plans/shipped/pillar-1-2-perf-review-system.md`. Diff touches `Source/Core/` (Tracker strict zone, P4, Config, Ui) → gates **fire**.

1. **PR-fast CI** — scenario closest to the changed path: a grid/issue-view scenario for Slice 3 UI draw; backend slices exercise tracker-fetch scenarios. Map: `agents/core/perf-gatekeeper.md` § Curated diff → scenario map; declare the subset in `scripts/dev/perf-pr-fast-set.json` if the window opens a new scenario.
2. **Pillar 2 static scanner** — **fires.** New fetches (p4 changes, JQL activity, group members, CL describe) must be worker-thread; annotate any unavoidable sync path `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms`. Default design = all async, scanner passes clean.
3. **Dispatcher drain** — async results post back via `MainThreadDispatcher` (as `SmatchetAuditUi` does); does **not** modify `Drain()` itself → N/A to the drain-edit gate, but the new post-backs must be drain-safe.
4. **Visible-cue bucket-E harness** — activity/p4 loads are the new >100ms paths; each carries a visible cue (progress bar / disabled button / "Loading…") → add a bucket-E assertion that the cue shows.
5. **Marker inventory** — if `SMATCHET_UI_PERF_SCOPE` markers are added to the new window draw, regen `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against the named scenario(s) before opening each PR.

**Override**: `perf-out-of-band` label only for an intentional, baseline-bumped regression.

## Risks / non-goals

- **Activity-feed contract is self-designed** (JQL `WAS` + per-issue changelog scan, one row per user-authored item). Risk: Jira changelog volume / rate-limits for active users (N issues × 1 changelog fetch each). Mitigation: day-windowed query (spec 7.1), author + window filter bounds rows, progress counter; isolate in Slice 2 so UI doesn't block on tuning.
- **P4 username = email local-part** (spec 6.1) — users whose p4 id ≠ email local-part get no/incorrect changes. Accepted (spec edge-case §11) — surface via the "Loading changes for {p4user}…" log line so the derived id is visible.
- **Plane/GitHub degradation** — both return `nullptr` Collaboration today, so the tracker activity + groups sections are empty for them. Mitigation: window null-checks `Collaboration()`; identity + P4 sections still work. This is the chosen meaning of "Jira + Plane support" (grill Q1) — **no Plane/GitHub backend code in this plan**.
- **Shared-helper extraction touches working code** (`AnnotateAnalysisUi`). Risk: regressing the Annotate window's CL tooltip / p4v launch. Mitigation: pure extraction, no behaviour change; the Annotate window's existing bucket-E/scenario coverage must stay green after repointing (verify in Slice 1).
- **Date-window filter relies on `YYYY-MM-DD` string ordering** (spec 7.2, §11). Risk: a backend timestamp not leading with that format breaks the cutoff. Mitigation: keep the "shorter than 10 chars → keep unconditionally" rule; assert in bucket-A.
- **Deliberate spec deviation (add-to-query value)** — the activity Issue cell passes the parsed key, not the issue URL (spec 7.4), so add-to-query produces a valid JQL `key = "{key}"`. Recorded here so it isn't mistaken for an accidental drop; the URL is still used for context-menu browser-open.
- **Non-goals**: not adding a user-picker/search entry point (window is opened by an existing selection elsewhere); not enabling Plane/GitHub collaboration (separate plan); not redesigning the Annotate tooltip or p4v launcher beyond the extraction; not adding keyboard-grid-nav beyond Escape-to-close (Pillar 4 backlog).

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: (1) `p4 changes` output parse → `P4ChangeSummary` (CL/date/user/first-line); (2) email → p4-username local-part split; (3) estimate-seconds → `Xd Yh Zm` with 8h workday (zero-unit omission, parse-failure passthrough); (4) 6+-digit CL scan in details; (5) `YYYY-MM-DD` cutoff string-compare incl. the short-timestamp keep-rule; (6) changelog JSON → `TrackerActivityEntry` mapping; (7) production-group keyword case-insensitive "contains" match.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: window opens/closes (Escape + Close), Close/Escape triggers `ClearUserActivity` but host-`isOpen`-clear does not; load buttons disable while loading + tooltips; CL/issue-key/member click interactions per spec §10 interaction table; loading progress bar visible-cue assertion (perf gate 4); one-shot group-fetch fires at most once per open.
- **Bash-driver scenario / screenshot / sanitizer**: a scenario opening the window for a fixture user + screenshot diff of the three sections; sanitizer build clean over the new TUs.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — per slice, anchored to the new TUs added to both source lists.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (enumerates anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: **DONE 2026-06-06** — 6 forks resolved: (Q1) Plane=null-degrade, zero Plane code; (Q2) terms `TrackerActivityEntry` + "group roster" (not "catalog"), all in-memory — committed to Tracker `CONTEXT.md`; (Q3) activity row = one user-authored changelog item via `WAS` discovery; (Q4) add-to-query replaces JQL with `key="{key}"`, URL→browser-open only (deliberate spec deviation); (Q5) new `FindFirstIssueKeyInText` delegating to `ExtractIssueKeyPrefix`; (Q6) extract shared `P4ClPreview`/`P4vLaunch` helpers. No ADR warranted (in-memory/reversible; Plane-null covered by ADR-0012 + nullable-role contract).
- **Manual residue**: visual-validation exception applies (Slice 3 touches `Smatchet*Ui*.cpp`) — if no bucket-E/screenshot coverage lands for a visual aspect, the orchestrator pauses with the launched exe for user verdict + adds a `docs/self-improvement/categories/tooling.md` entry. No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here (Plane/GitHub collaboration enablement, user-picker entry point) and revise/delete them. New plan — nothing was previously marked "deferred-as-current", so the sweep is a confirmation, not a cleanup.

- **GitHub backend group/activity parity** — degrades to empty in v1; no follow-up planned unless requested.
- **User-picker / search-to-open entry point** — window is opened by an existing selection; a dedicated picker is a separate UX feature.
- **Plane/GitHub collaboration enablement** — flipping `PlaneClient::Collaboration()` off `nullptr` (turns on comments/watchers/votes/groups/activity for Plane) is its own backend plan, not this window.
- **Keyboard grid navigation** (beyond Escape-close) — Pillar 4 backlog, consistent with sibling windows.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/user-info-window.md docs/plans/shipped/` (move into the shipped tier),*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md`, so the move can't break them.*

*(Delete this `## Archive` block as part of step 2.)*
