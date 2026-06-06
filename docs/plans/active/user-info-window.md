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

Scope decisions (confirmed with user 2026-06-06): **Jira + Plane** in v1 (GitHub degrades gracefully); activity feed sourced via **JQL + changelog scan**; issue-key detection **reuses `ProjectResolver::ExtractIssueKeyPrefix`** (not a new `[A-Z]{2,10}-[0-9]+` regex); delivered as **3 sliced PRs**.

## Approach

**Three slices, each its own PR** (per § PR-batching — split along seams; the full diff would exceed the CodeRabbit per-PR file ceiling):

**Slice 1 — P4 changes-by-user + config keys (foundation, no UI).**
Add `P4ChangesForUser(cfg, p4user, maxN) -> vector<P4ChangeSummary>` to the P4 layer (new `p4 changes -u <user> -m N -s submitted` invocation, reusing `P4RunCommand`). Add two `ConfigManager` keys via the existing `kStringFields`/`kIntFields` value-lookup pattern: `ProductionGroupKeyword` (string) and `UserActivityDayWindow` (int, default e.g. 30). Add `MaxUserChanges` constant (app-wide N). Pure-logic parsing of `p4 changes` output is bucket-A testable.

**Slice 2 — tracker backend: activity feed + group members + group catalog (Jira + Plane).**
Extend `ITrackerCollaboration` with: `FetchUserActivity(accountId, dayFrom, dayTo, progress&, out&, err&)` returning a new `TrackerActivityEntry` struct (timestamp, issueKey, issueUrl, summary, actionLabel, details) sourced via JQL (`assignee was X OR reporter was X` over the window) + per-issue `expand=changelog` parse; `FetchGroupMembers(groupName, out&, err&)`; `ClearUserActivity()`; and a progress counter (current/total) surfaced for the loading bar. A global group-catalog cache (group name → member list, loaded lazily) lives in the collaboration layer / `AppController`. Jira implements fully; Plane implements what its API exposes and **degrades gracefully** (empty groups / empty activity rather than erroring) per spec §12; GitHub inherits empty defaults. Delegate to `tracker-backend`.

**Slice 3 — the UI window.**
New `SmatchetUserInfoUi` (header + P4 section + activity section + groups section + shared issue-key context-menu popup), following the `AnnotateAnalysisUi` / `SmatchetPerfUi` full-window pattern: `DrawWindow(AppController&, UiDrawSession&, bool* pOpen)`, registered in `SmatchetUI::drawSecondaryWindows()`. Reuse `DrawClTooltipAsync` for CL previews and `LaunchP4VcLike` for open-in-p4v. Host callbacks `onStatus` / `onAddToQuery` thread through `UiDrawSession` (add-to-query pushes into the active view JQL the same way `SmatchetActiveProjectGridUi` mutates `cfg.JqlQuery`). One-shot group-fetch flag, Escape/Close → `ClearUserActivity`, per-render group resolution. Delegate to `grid-engine` / general UI.

Trade-off: activity feed is the riskiest surface (self-designed JQL+changelog contract). It is isolated in Slice 2 so the UI (Slice 3) can be built against a populated or empty feed without blocking on backend tuning.

## Files to modify

> Grepped: no existing `UserInfo*` / `SmatchetUserInfoUi` TU (investigation confirmed 0 defs). `TrackerActivityEntry`, `P4ChangesForUser`, `FetchUserActivity`, `FetchGroupMembers` do not exist under any synonym.

**Slice 1 — P4 + config**
1. [P4Annotate.h](Source/Core/include/P4Annotate.h:30) — add `P4ChangeSummary` struct (CL number, date, user, first-line description) + `P4ChangesForUser` decl.
2. [P4Annotate.cpp](Source/Core/src/P4Annotate.cpp:112) — implement `P4ChangesForUser` via `p4 changes -u <user> -m N -s submitted`, reusing `P4RunCommand`; parse into `P4ChangeSummary`.
3. [ConfigManager.h](Source/Core/include/Config/ConfigManager.h:509) — add `ProductionGroupKeyword` (string), `UserActivityDayWindow` (int), `MaxUserChanges` (int) fields.
4. [ConfigManager.cpp](Source/Core/src/Config/ConfigManager.cpp:743) — register the new keys in the `kStringFields` / int value-lookup tables with defaults.

**Slice 2 — tracker backend (strict zone `Source/Core/src/Tracker/`)**
5. [ITrackerCollaboration.h](Source/Core/include/ITrackerCollaboration.h:62) — add `TrackerActivityEntry` struct, `TrackerActivityProgress` (current/total), and virtual methods `FetchUserActivity`, `FetchGroupMembers`, `ClearUserActivity` (default-empty impls so Plane/GitHub degrade).
6. [JiraClient.h](Source/Core/include/Tracker/JiraClient.h:146) — declare the three new overrides.
7. [JiraUserAndMeta.cpp](Source/Core/src/Tracker/JiraUserAndMeta.cpp:366) — implement `FetchGroupMembers` (GET group members) + activity via JQL search + `expand=changelog` parse.
8. New `Source/Core/src/Tracker/JiraActivityFeed.{h,cpp}` (or fold into `JiraIssueSearch.cpp`) — JQL `assignee was X OR reporter was X` window query + changelog → `TrackerActivityEntry` mapping, progress counter.
9. [PlaneClient.h](Source/Core/include/Tracker/PlaneClient.h:16) + Plane impl — implement what Plane's API exposes; explicit graceful-degrade (empty) for the rest.
10. [AppController.h](Source/Core/include/AppController.h:948) + `AppController_CatalogAndFieldEdit.cpp:1609` — delegator methods + lazy global group-catalog cache (group → members).

**Slice 3 — UI**
11. New `Source/Core/include/Ui/SmatchetUserInfoUi.h` + `Source/Core/src/Ui/SmatchetUserInfoUi.cpp` — the window (light zone; ImGui-draw 200-line cap; split into `_Sections.cpp` if it grows).
12. [SmatchetUI.h](Source/Core/include/Ui/SmatchetUI.h:193) — own the `SmatchetUserInfoUi` instance + declare in `drawSecondaryWindows()`.
13. [SmatchetUI.cpp](Source/Core/src/Ui/SmatchetUI.cpp:193) — register render call.
14. [SmatchetUiSession.h](Source/Core/include/Ui/SmatchetUiSession.h:139) — thread `onAddToQuery` / `onStatus` host callbacks (or reuse the existing JQL-mutation path).
15. CMake source-list gating — add the new TUs to both `SmatchetStandalone` and `SmatchetCore_DX12` lists (dual-target).

## Existing utilities reused

- `DrawClTooltipAsync` — [AnnotateAnalysisUi_Modals.cpp:432](Source/Core/src/Ui/AnnotateAnalysisUi_Modals.cpp:432) — shared Annotate changelist-preview tooltip; reuse for CL cells (spec 6.2, 7.5) — do not reimplement.
- `LaunchP4VcLike` — [AnnotateAnalysisUi_Launch.cpp:122](Source/Core/src/Ui/AnnotateAnalysisUi_Launch.cpp:122) — open a changelist in p4v.
- `P4RunCommand` — [P4Annotate.cpp:29](Source/Core/src/P4Annotate.cpp:29) — p4 invocation wrapper for the new changes-by-user query.
- `P4ChangelistDescribeCache` — [P4Annotate.h:62](Source/Core/include/P4Annotate.h:62) — LRU describe cache feeding the preview tooltip.
- `ProjectResolver::ExtractIssueKeyPrefix` — [ProjectResolver.cpp:21](Source/Core/src/Tracker/ProjectResolver.cpp:21) — issue-key detection (per user decision: reuse, not a new regex). Note the asymmetry the spec mandates: activity Issue cell (7.4) operates on issue **URL**, Details cell (7.5) on parsed **key** — preserve.
- `JiraClient::FetchUserGroupNames` / `SearchUsersByQuery` — [JiraUserAndMeta.cpp:366](Source/Core/src/Tracker/JiraUserAndMeta.cpp:316) — existing group-name + user lookup; activity/members build alongside.
- `TrackerUser` (AccountId / DisplayName / EmailAddress) — [TrackerFieldSchema.h:60](Source/Core/include/Tracker/TrackerFieldSchema.h:60) — identity fields; p4 username derives from email local-part (spec 6.1, edge-case §11).
- `AnnotateAnalysisUi` / `SmatchetPerfUi` full-window pattern — [AnnotateAnalysisUi.h:18](Source/Core/include/Ui/AnnotateAnalysisUi.h:18) — `DrawWindow(...)` + `drawSecondaryWindows()` registration template.
- Active-query JQL mutation — [SmatchetActiveProjectGridUi.cpp:532](Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp:532) — pattern for `onAddToQuery` to push a key/URL into the active view.

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

- **Activity-feed contract is self-designed** (JQL + changelog scan). Risk: Jira changelog volume / rate-limits for active users. Mitigation: day-windowed query (spec 7.1), bounded fetch, progress counter; isolate in Slice 2 so UI doesn't block on tuning.
- **P4 username = email local-part** (spec 6.1) — users whose p4 id ≠ email local-part get no/incorrect changes. Accepted (spec edge-case §11) — surface via the "Loading changes for {p4user}…" log line so the derived id is visible.
- **Plane degradation** — Plane may not expose group membership / assignee-reporter history identically. Mitigation: explicit empty-degrade (no error) per spec §12; document which methods are no-ops on Plane.
- **Issue-key detection via `ExtractIssueKeyPrefix`** diverges from the spec's literal `[A-Z]{2,10}-[0-9]+`. Accepted (user decision) — gives consistency with the rest of the app and avoids Plane mis-detection; verify it still detects Jira keys in the description/details first lines.
- **Date-window filter relies on `YYYY-MM-DD` string ordering** (spec 7.2, §11). Risk: a backend timestamp not leading with that format breaks the cutoff. Mitigation: keep the "shorter than 10 chars → keep unconditionally" rule; assert in bucket-A.
- **Non-goals**: not adding a user-picker/search entry point (window is opened by an existing selection elsewhere); not adding GitHub group/activity parity (degrades empty); not redesigning the Annotate tooltip or p4v launcher (reused as-is); not adding keyboard-grid-nav beyond Escape-to-close (Pillar 4 backlog).

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: (1) `p4 changes` output parse → `P4ChangeSummary` (CL/date/user/first-line); (2) email → p4-username local-part split; (3) estimate-seconds → `Xd Yh Zm` with 8h workday (zero-unit omission, parse-failure passthrough); (4) 6+-digit CL scan in details; (5) `YYYY-MM-DD` cutoff string-compare incl. the short-timestamp keep-rule; (6) changelog JSON → `TrackerActivityEntry` mapping; (7) production-group keyword case-insensitive "contains" match.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: window opens/closes (Escape + Close), Close/Escape triggers `ClearUserActivity` but host-`isOpen`-clear does not; load buttons disable while loading + tooltips; CL/issue-key/member click interactions per spec §10 interaction table; loading progress bar visible-cue assertion (perf gate 4); one-shot group-fetch fires at most once per open.
- **Bash-driver scenario / screenshot / sanitizer**: a scenario opening the window for a fixture user + screenshot diff of the three sections; sanitizer build clean over the new TUs.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — per slice, anchored to the new TUs added to both source lists.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (enumerates anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms (TrackerActivityEntry vs existing comment/changelog structs; "group catalog" vs existing field-catalog naming; Annotate-tooltip reuse) before finalising; record outcome. Required — do not delete.
- **Manual residue**: visual-validation exception applies (Slice 3 touches `Smatchet*Ui*.cpp`) — if no bucket-E/screenshot coverage lands for a visual aspect, the orchestrator pauses with the launched exe for user verdict + adds a `docs/self-improvement/categories/tooling.md` entry. No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here (GitHub group/activity parity, user-picker entry point) and revise/delete them.

- **GitHub backend group/activity parity** — degrades to empty in v1; no follow-up planned unless requested.
- **User-picker / search-to-open entry point** — window is opened by an existing selection; a dedicated picker is a separate UX feature.
- **Plane full activity parity** — if Plane's API can't express assignee/reporter history, that gap is a follow-up backend plan, not this one.
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
2. *`git mv docs/plans/active/user-info-window.md docs/plans/shipped/user-info-window.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md`, so the move can't break them.*

*(Delete this `## Archive` block as part of step 2.)*
