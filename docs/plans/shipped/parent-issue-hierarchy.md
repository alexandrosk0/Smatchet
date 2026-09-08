# Plan — Parent issue hierarchy (FS parity)
<!-- plan-date: 2026-09-06 -->

> **Slug**: `parent-issue-hierarchy`
>
> **Status**: `shipped`
>
> **Origin**: user request 2026-09-06 — "look into how parent jiras are loaded, I want to duplicate the functionality from `C:\Dev\FS`". Scope pinned by user: **full FS parity**, **all backends via the `ITrackerIssueReader` interface**, **per-view flag**.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The sibling FS app (`C:\Dev\FS`, `fairbairn.cpp` / `jira_service.cpp` / `sykes.cpp`) loads a view's issues and then, in the same query session, fetches every **parent issue referenced but not present** in the result set (`Fairbairn::FetchMissingParents`, one `key IN (...)` search). The grid then offers a **story-group sort** (parent row followed by its children, depth-indented, parent rows tinted) and a per-view **"Hide parent stories"** toggle that skips the parent fetch and filters out any row that other rows reference as a parent. When the quick-filter is active, filtered-in children pull their ancestor chain back in so context is never lost.

Smatchet already parses the parent field into `CachedTicket.fieldValues["parent"]` as `"KEY - summary"` and already has a backend-agnostic `FetchIssuesForKeys` primitive, but nothing calls it after a view sync, the grid has no hierarchy concept, and there is no per-view flag. After this lands: a view sync appends missing parents to the cache/grid, the grid can render parent-then-children groups with indent + tint, and each view carries a persisted `Hide parent stories` switch that mirrors FS.

FS reference points (read-only; Smatchet idioms win over verbatim copies):

| FS | Behaviour | Smatchet equivalent |
|---|---|---|
| `fairbairn.cpp:1578` ParseIssue | `parent.key + ": " + summary`, fallback `issuelinks` inward "is part of" | `TrackerFieldValueParser.cpp:819` `NormalizeParentRefObject` (called `:885`) emits `"KEY - summary"`; no issuelinks fallback yet |
| `fairbairn.cpp:648` `ExtractParentId` | substring before `:` | new `ParentKeyFromFieldValue` (substring before `" - "`) — dedupes the 3 inline splits |
| `fairbairn.cpp:2220` `FetchMissingParents` | referenced − present → `key IN (...)` | new `MissingParentKeys` + `FetchIssuesForKeys` in the sync worker |
| `jira_service.cpp:47` `StartQuery(jql, fetchParents)` | flag skips the fetch | `ViewDefinition::HideParents` read in `RunStreamingWorkerBody` |
| `fairbairn.cpp:2353` `SortResults` (`ByStoryGroup` branch `:2366`) | parent then children DFS | new `StoryGroupOrder` applied over `cachedSortedIndices` |
| `sykes.cpp:422` `CalculateDepth` | ancestor walk with visited-set | `ComputeDepths` (bounded, cycle-safe) |
| `sykes.cpp:2343` filter | hide referenced parents; filter-text re-adds ancestor chain | filter step in `RebuildGridSortAndFilterProjection` |
| `sykes.cpp:3352` render | indent `depth*20`, `ParentRow` / `ParentRowNested` tint | `SmatchetActiveProjectGridTable.cpp:921` row-bg hook + Id-cell indent |
| `config_service.h:43` colours | `ImVec4(0.5,0.3,0.6,0.4)` / `(…,0.2)` | new per-theme palette slots on `SmatchetTheme.h` (light theme gets its own pair) |

## Approach

Three slices on one branch, one PR (`AGENTS.md` § PR batching).

**Slice 1 — fetch (Sync + Tracker, strict zones).** A new pure TU `ParentHierarchyPure` (`Source/Core/{include,src}/Tracker/`) exposes `ParentKeyFromFieldValue`, `ReferencedParentIds`, `MissingParentKeys` (referenced − present, deduped, stable order), `ComputeDepths`, `StoryGroupOrder`, `AncestorChain`. `TicketSyncService::RunStreamingWorkerBody` (`Source/Core/src/Sync/TicketSyncService.cpp:766`) accumulates each streamed batch's parent references alongside `workerKeepIds`; after `FetchIssuesStreamed` (`:786`) returns with an empty `FetchError`, not cancelled, and the active view's `HideParents == false` — and **before** the `localStaleIds` computation at `:788-797`, so the fetched ids are in `workerKeepIds` when the stale purge is derived — it calls `deps_.Backend()->FetchIssuesForKeys(cfgCopy, missing, viewsCopy)` **on the same worker thread**, pushes the result as one more `PendingBatches` entry, and inserts the fetched ids into `workerKeepIds` so the full-sync stale purge does not delete them next tick. One level only (FS parity, no grandparent recursion). A failed parent fetch degrades to `summary.Warning` ("N parent issues could not be loaded") and never flips the main sync to error. Any backend whose parser emits `parent` benefits; a backend that never does yields an empty `missing` set and skips the call. No new interface method: `FetchIssuesForKeys` already lives on `ITrackerIssueReader`, so nothing Jira-shaped leaks. The `issuelinks` "is part of" fallback goes into the Jira branch of `TrackerFieldValueParser` (Jira detail stays inside the Jira parser, as today).

**Slice 2 — view flag + persistence (Config, strict zone).** `ViewDefinition` gains `bool HideParents = false` and `bool StoryGroupSort = false`; `ConfigManager_Views.cpp` reads/writes `"hide_parents"` / `"story_group_sort"` (missing key → default, so existing `smatchet_views.json` files load unchanged; an older build ignores the unknown keys). View-dirty detection needs no change: it is a whole-struct snapshot copy (`SnapshotActiveViewIfNeeded`) with no equality / fingerprint path, so the new members are covered automatically (item 11). Toggles live in the existing **Sort By** popup (`DrawSortByPopupBody`, `SmatchetGridHeaderUi.cpp:92`): a `Story group` checkbox and a `Hide parent stories` checkbox with FS's tooltip text, both localised.

**Slice 3 — grid projection + render (Ui, light zone).** `RebuildGridSortAndFilterProjection` (`SmatchetActiveProjectGridTable.cpp:169`) gains: (a) when `StoryGroupSort`, post-process `cachedSortedIndices` through `StoryGroupOrder` — parents keep the column sort among themselves, children keep it within their group, matching FS's `ByStoryGroup`; (b) the filter step honours `HideParents` (drop rows whose id is in the referenced-parent set) and, when quick-filter text is active with `StoryGroupSort` on, re-adds each match's ancestor chain (FS `sykes.cpp:2380`). Per-pane cache gains `cachedDepths` + `cachedParentIds` rebuilt in the same revision-gated call, so steady-state frames pay nothing. Render: in the row loop at `SmatchetActiveProjectGridTable.cpp:921`, when `StoryGroupSort` is on, parent rows get `TableSetBgColor(RowBg0, ParentRowBg / ParentRowNestedBg)`. Precedence is a Smatchet-local decision: FS has no status row tint (its order is duplicate-source > find-hit > selected > parent tint, `sykes.cpp:3400-3420`), so here the existing status tint (`StatusRowColor`, alpha 0.12) keeps priority and the parent tint applies only to rows without a status colour and the Id cell gets `ImGui::Indent(depth * kIndentPx)` / `Unindent`.

Named trade-off: the parent fetch adds one extra HTTP round-trip **per view sync** on the worker thread, gated per view. That is the FS behaviour the user asked for; the existing streaming-sync cue covers it (Pillar 2), and an empty `missing` set costs nothing.

## Files to modify

### Tracker (strict zone — read `Source/Core/src/Tracker/AGENTS.md` first)
1. **NEW** `Source/Core/include/Tracker/ParentHierarchyPure.h` + `Source/Core/src/Tracker/ParentHierarchyPure.cpp` — pure helpers listed above. Grepped: no `ParentHierarchy*` / `StoryGroup*` / `IssueHierarchy*` TU exists. No ImGui / HTTP includes; unit-testable. No CMake registration needed: `Source/Core/CMakeLists.txt:30` uses `file(GLOB_RECURSE … CONFIGURE_DEPENDS "Source/Core/src/*.cpp")`, so a re-configure picks the new TU up.
2. `Source/Core/src/Tracker/TrackerFieldValueParser.cpp:819` `NormalizeParentRefObject` (call site `:885`) — when the `parent` object path yields empty, scan `issuelinks[].type.inward ∈ {"is part of", "part of"}` → `inwardIssue.key (+ " - " + summary)` (FS `ExtractParentFromLinks`, `fairbairn.cpp:768`). Same `"KEY - summary"` shape so every consumer keeps working.
3. `Source/Core/src/Tracker/IssueDraft.cpp:158`, `Source/Core/src/Tracker/TrackerFieldPayloadPure.cpp:356` (inside `ExtractIssueKey`), `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp:51` — replace the three inline `find(" - ")` splits with `ParentKeyFromFieldValue` (DRY; a fourth copy would trip the dup gate). `ExtractIssueKey` also runs a `LooksLikeIssueKey` pre-check on the whole string and on the split key; keep that validation in place and only swap the split itself.
4. `Source/Core/src/Tracker/JiraIssueMappingPure.cpp:101-102` (`BuildFetchFieldListsFromView` fixed list) — add `issuelinks` to the fixed fetch field list (absent today; without it the fallback never sees data). Links are small; payload impact negligible.
5. `Source/Core/src/Tracker/AGENTS.md` — one bullet under `## Invariants` (the doc has only `## Invariants` / `## Before you edit`): `FetchIssuesForKeys` is also the missing-parent path from `TicketSyncService`. Documentation only — the `interface-doc` WARN gate fires solely when a pinned header symbol changes, and `ITrackerIssueReader.h` is untouched here.

### Sync (strict zone — read `Source/Core/src/Sync/AGENTS.md` first)
6. `Source/Core/src/Sync/TicketSyncService.cpp:766` `RunStreamingWorkerBody` — collect parent refs per batch inside `onBatch`; after `FetchIssuesStreamed` (`:786`) and before the `localStaleIds` derivation (`:788-797`), if `FetchError` non-empty or `shouldCancel()` (lambda `:781`) → skip; else resolve `HideParents` from `viewsCopy`'s active view, compute `missing`, call `FetchIssuesForKeys`, push batch under `QueueMutex` with the same `RequestId == reqId && !Cancelled` guard, extend `workerKeepIds`, set `summary.Warning` on failure. Extract to a private `FetchMissingParentsIntoQueue(...)` so the worker body stays under the 120-line cap.
7. `Source/Core/include/Sync/TicketSyncService.h:125` (private section starts `:89`) — declare the new private method.
8. `Source/Core/src/Sync/AGENTS.md` — append a fourth bullet under `## Invariants` (today: offline queue, replay reuse, audit pair): *parents fetched in the same worker session are part of `KeepIds`; a parent-fetch failure is a Warning, never a FetchError.*

### Config (strict zone)
9. `Source/Core/include/Config/ConfigManager.h:575` `ViewDefinition` (Id, Name, Jql, Fields, ColumnOrder, ColumnWidths, SortSpecs today) — add `bool HideParents = false; bool StoryGroupSort = false;` with a doc comment pointing at this plan.
10. `Source/Core/src/Config/ConfigManager_Views.cpp:39` `ParseViewDefinition` + `Source/Core/src/Config/ConfigManager_Views.cpp:110` `SerializeView` — `"hide_parents"` / `"story_group_sort"` via `.value(key, default)`.
11. View dirty / Discard — **verify-only, no code**: there is no `ViewDefinition::operator==` or fingerprint. Dirty state is the `viewsHasOriginalSnapshot` flag plus a whole-struct copy in `viewsOriginalSnapshot` (`SnapshotActiveViewIfNeeded`, `SmatchetViewsDashboardUi_widgets.cpp:29`); Discard does `*mutableActive = d.viewsOriginalSnapshot` (`SmatchetActiveProjectGridUi.cpp:609-610`, `SmatchetViewsDashboardUi.cpp:1086-1087`). The two new bools ride along automatically; Save persists them via `SerializeView`. Confirm with the bucket-E toggle test.

### Ui (light zone — visual-validation pause applies)
12. `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp:92` `DrawSortByPopupBody` — two checkboxes above the sort-rule list: `Story group` (tooltip "Group children under their parent story") and `Hide parent stories` (tooltip "Hide parent stories (show only leaf tasks/bugs)", FS `sykes.cpp:5635`). Each calls `SnapshotActiveViewIfNeeded` first (existing call at `:152`), flips the view field, sets `sortChanged`. Strings go through `SmatchetLocalization::T(key, fallback)` (the TU already does this, e.g. `grid.sort.remove_key` at `:109`); register the keys as table rows in `Source/Core/src/SmatchetLocalization.cpp` with English + French columns (`{"grid.sort.story_group", "Story group", u8"…"}` pattern, cf. `:1157`).
13. `Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp:169` `RebuildGridSortAndFilterProjection` (static) — story-group post-order, `HideParents` filter, ancestor re-add, `cachedDepths` / `cachedParentIds` rebuild. The function is not `Draw*`-named, so the **120-line** cap applies; split the filter step into `ApplyGridRowFilter(...)` if it crosses.
14. `Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp:917-925` row loop (`TableNextRow` + status `TableSetBgColor`) — parent-row tint + Id-cell indent (depth passed via the existing `ActiveProjectDrawCtx` rather than widening the `drawActiveProjectGridCell` signature).
15. `Source/Core/include/GridPane.h:67` — `std::vector<int> cachedDepths; std::unordered_set<std::string> cachedParentIds;` beside the existing projection cache.
16. `Source/Core/include/Ui/SmatchetTheme.h:62` `SmatchetThemeSemanticColors` (today only `ErrorText` / `WarningText` / `SuccessText`) + `Source/Core/src/Ui/SmatchetTheme.cpp:888` `BuildSemanticColorsForTheme(ThemeId)` — add `ParentRowBg` / `ParentRowNestedBg` slots, update the struct doc comment (it currently describes status-text colours only) and fill them in every theme branch of the builder (dark: FS values `(0.5,0.3,0.6,0.40)` / `(…,0.20)`; light: darker lavender at lower alpha; confirm AA contrast of body text over both).

### Tests
17. **NEW** `tests/Core/ParentHierarchyPure.test.cpp` — bucket A (see § Verification).
18. `tests/Core/TicketSyncService.test.cpp` — worker-drain cases for the parent fetch (fixture backend already implements `FetchIssuesForKeys`, `TrackerFixtureBackendBase.cpp:66`).
19. `tests/Core/ConfigManagerViews.test.cpp` — round-trip of the two new keys + default-on-missing.
20. `tests/CMakeLists.txt` — register the new test TU (explicit source list, no glob — unlike `Source/Core`).

## Existing utilities reused

- `ITrackerIssueReader::FetchIssuesForKeys` — `Source/Core/include/ITrackerIssueReader.h:69-71`; backend-agnostic keyed fetch, already used by `Source/Core/src/AppController_TicketPrefetch.cpp:125` and `Source/Core/src/Sync/OfflineQueueService.cpp:966`. **Not** a new Jira search call.
- `JiraClient::FetchIssuesForKeys` — `Source/Core/src/Tracker/JiraIssueSearch.cpp:443`; dedupes keys and builds the same field list (`BuildFetchFieldListsFromView`) as the view sync, so parents carry the view's columns.
- `TrackerFieldValueParser` parent shape — `Source/Core/src/Tracker/TrackerFieldValueParser.cpp:819` `NormalizeParentRefObject`; the `"KEY - summary"` contract every consumer already splits on.
- `activeStreamingSync_.PendingBatches` + `KeepIds` drain — `Source/Core/src/Sync/TicketSyncService.cpp:363` / `:395`; one more batch reuses the transactional `SaveTickets` + stale-purge bookkeeping unchanged.
- `TrackerIssueFetchSummary::Warning` — `Source/Core/include/ITrackerIssueReader.h:26-28`; the "useful data with a caveat" channel, exactly the parent-fetch-failed semantics.
- `RebuildGridSortAndFilterProjection` per-pane cache + revision gating — `Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp:169`; hierarchy work rides the existing dirty check.
- `SnapshotActiveViewIfNeeded` — defined `Source/Core/src/Ui/SmatchetViewsDashboardUi_widgets.cpp:29` (decl `SmatchetViewsDashboardUi_detail.h:222`), called from `SmatchetGridHeaderUi.cpp:152`; the undo-snapshot every view mutation calls first.
- `StatusRowColor` row tint precedent — `Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp:855` (applied `:919-925`); same `TableSetBgColor(RowBg0, …)` idiom.
- `CompareIssueKeyNatural` (`Source/Core/include/StringUtil.h:70`) — natural key order for top-level parents when no other sort spec is active (FS sorts by numeric id first).
- `TrackerFixtureBackendBase::FetchIssuesForKeys` — `Source/Core/src/Tracker/TrackerFixtureBackendBase.cpp:65-66`; the test double already serves keyed fetches from its fixture set.

## Extraction sizing

N/A — no over-cap file is split; the new pure TU is additive.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: order / depth / parent-set computed only inside the revision-gated projection rebuild; per-row render cost is one `unordered_set::find` + one indent when `StoryGroupSort` is on, zero when off. `priority-grid-scroll` re-run before PR.
- **Pillar 2 (UI never freezes)**: the parent fetch runs on the existing streaming worker thread under the existing "Refreshing issues…" cue; no new sync I/O reachable from `ImGui::*`.
- **Pillar 3 (never crash)**: `ComputeDepths` / `StoryGroupOrder` / `AncestorChain` use a visited-set and a hard bound (`kMaxHierarchyDepth = 64`) so a parent cycle in backend data cannot recurse unbounded; all index lookups bounds-checked against `tickets.size()`. Parent-fetch exceptions stay inside the worker's existing try/catch.
- **Pillar 4 (accessibility)**: both toggles are ImGui checkboxes (keyboard-navigable); parent-row tint is per-theme with a light-theme pair so text stays AA over it; indent uses `ImGui::Indent` so font scaling applies.

## Perf-review-system gates

1. **PR-fast CI** — per `agents/core/perf-gatekeeper.md` § Curated diff → scenario map: `SmatchetGrid*.cpp` / `SmatchetActiveProjectGridTable.cpp` → `priority-grid-scroll` + `cell-edit-burst`; `SmatchetTheme.cpp` → `idle`; `Sync/TicketSyncService.cpp` has **no map entry** → falls back to `idle` and the gatekeeper flags the gap to `test-author`. `concurrent-sync` is already in `scripts/dev/perf-pr-fast-set.json` (idle, priority-grid-scroll, cell-edit-burst, ai-chat-history-render, side-by-side-2-grid, concurrent-sync) and exercises the sync path, so no new scenario is needed; add a `Sync/` map row if the gatekeeper asks.
2. **Pillar 2 static scanner** (`scripts/dev/pillar2-scan.sh`, scans UI-reachable files for cpr / SQLite / popen / ifstream / `mutex.lock`) — expected N/A: the fetch goes through the backend interface, `TicketSyncService.cpp` carries no cpr call and has no annotation today. If the scanner does flag the new call site, annotate with the grammar `/* PILLAR2_WORKER_ONLY */ // est-latency: 300ms`.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new > 100 ms stall path on the UI thread.
5. **Marker inventory** — N/A: no new `SMATCHET_UI_PERF_SCOPE` markers; existing `activeProject:grid.*` scopes cover the changed code.

**Pre-push local check**: `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against `priority-grid-scroll`.

## Risks / non-goals

- **Stale-purge deletes fetched parents** if their ids miss `KeepIds` — mitigated by inserting them into `workerKeepIds` before the summary is published; a bucket-A test pins it.
- **Parent fetch after a cancelled / superseded request** wastes a call — mitigated: check `shouldCancel()` before issuing it; the drain already drops batches whose `RequestId` moved on.
- **Views JSON compat**: unknown keys are ignored by older builds; missing keys default in this build. Accepted.
- **Backends without a `parent` field** (GitHub today; Plane read-side): `missing` is empty, no call, toggles inert. Accepted — plumbing is generic so those backends light up when their parsers emit `parent`.
- **Row count grows after the main batches** when parents append: the projection already handles revision bumps from streaming; the parent batch is one more.
- **Non-goals**: no grandparent recursion (FS is single-level); no parent editing / drag-reparent (write side `IssueDraft.ParentKey` untouched); no collapse / expand tree UI; no change to the `"KEY - summary"` value contract.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `ParentHierarchyPure.test.cpp` — `ParentKeyFromFieldValue` (`"ABC-1 - x"` → `ABC-1`; no separator → whole string; empty → empty); `MissingParentKeys` (referenced − present, dedup, stable order, empty when all present); `ComputeDepths` (chain of 3 → 0/1/2; missing ancestor counts only present ancestors like FS; cycle A→B→A terminates with bounded depth); `StoryGroupOrder` (parent precedes children, orphans stay top-level, input order preserved within groups); `AncestorChain` (re-adds chain once, no duplicates). `TicketSyncService.test.cpp` — fixture backend with a child referencing an absent parent: drain yields both rows, parent id in `KeepIds`, full-sync purge does not delete it; `HideParents = true` → no parent fetched; fixture `FetchIssuesForKeys` error → `Warning` set, `FetchError` empty, child rows still applied. `ConfigManagerViews.test.cpp` — round-trip + defaults.
- **Bucket E (ImGui Test Engine, `tests/ui/*.test.cpp`, `IM_REGISTER_TEST` — pattern `grid_pane_windows.test.cpp` / `omnibar_search_apply.test.cpp`; note `tests/Core/GridHeaderToolbarLayout.test.cpp` is a bucket-A doctest, not the pattern)**: new `tests/ui/grid_parent_hierarchy.test.cpp` — toggle `Story group` in the Sort By popup, assert view dirty + `StoryGroupSort == true`; scenario-driven grid with fixture parent / child asserts row order parent-first and Id-cell indent > 0 for the child.
- **Bash-driver scenario / screenshot / sanitizer**: `priority-grid-scroll` perf gate-check; sanitizer preset build of the new TUs.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Lint gate**: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` (strict zones touched: Tracker, Sync, Config).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run before finalising; record outcome in § Deviations.
- **Manual residue**: parent-row tint + indent legibility on dark and light themes → visual-validation pause (exception 5) with the launched exe; deferred-automation: golden-image scenario for a fixture parent / child grid (`tests/golden/`), entry in `docs/self-improvement/categories/tooling/`.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray references to anything deferred here before finalising.

- **Multi-level ancestry fetch** (grandparents) — follow-up if users ask; FS does not do it.
- **Collapse / expand tree nodes** — FS has none; would need a per-pane collapsed-set. Follow-up plan candidate.
- **Parent re-assignment from the grid** (drag child onto parent) — write side exists (`IssueDraft.ParentKey`), UI not designed here.
- **Plane / GitHub parent parsing** — their read-side parsers do not emit `parent` today; adding it is a per-backend field-mapping slice, not hierarchy work.

## Implementation log

Shipped in one PR on branch `claude/parent-jiras-loading-b8f99c` (2026-09-07), all three slices together:

- **Tracker**: new `ParentHierarchyPure.{h,cpp}` (`kMaxHierarchyDepth = 64`; `ParentKeyFromFieldValue`, `ParentKeyOf`, `ReferencedParentIds`, `MissingParentKeys`, `PresentParentIds`, `AncestorChain`, `ComputeDepths`, `StoryGroupOrder`). `IssueDraft.cpp`, `TrackerFieldPayloadPure.cpp`, `SmatchetNewIssueDraftUi.cpp` now split the `"KEY - summary"` value through `ParentKeyFromFieldValue`. `JiraIssueMappingPure.cpp` adds `issuelinks` to the fixed fetch list; the issuelinks "is part of" fallback runs as a post-loop helper in the Jira mapper (row 2 in § Files to modify named the parser TU; the mapper owns the per-issue field loop, so the fallback landed where the parsed `parent` field is finalised).
- **Sync**: `TicketSyncService::FetchMissingParentsIntoQueue` (private) runs after `FetchIssuesStreamed`, skips on `FetchError` / cancel / `HideParents`, resolves the active view via the new `ConfigManager::FindActiveViewOrFirst`, pushes the parent batch under the same `RequestId` guard, extends `workerKeepIds`, and downgrades a keyed-fetch failure to `summary.Warning`. `TrackerIssueFetchSummary` is forward-declared in the header.
- **Config**: `ViewDefinition::HideParents` / `StoryGroupSort` (`"hide_parents"` / `"story_group_sort"`, default false); `ConfigManager::FindActiveViewOrFirst(views, activeViewId)` shared by the sync worker and `AppController::ResolveActiveViewProjectKeyForCatalog`.
- **Ui**: Sort By popup gains the `Story group` / `Hide parent stories` checkboxes (loc keys `grid.sort.story_group` / `grid.sort.hide_parents` + `.tip`, EN + FR). `RebuildGridSortAndFilterProjection` takes `GridHierarchyOptions` (story-group post-order, hide-parents filter, filter-text ancestor re-add, `cachedDepths` / `cachedParentIds`); the filter step is its own helper. Row loop: parent-row tint (`ParentRowBg` / `ParentRowNestedBg` semantic colours, status colour wins) + Id-cell indent `depth * 20`. The projection fingerprint carries `H` / `G` so a toggle rebuilds even when the column sort specs did not move; `BuildGridSortFingerprint` + `HierarchyOptionsForView` were extracted from `drawActiveProjectGridSort`.
- **Tests**: `tests/Core/ParentHierarchyPure.test.cpp` (new), worker-drain cases in `TicketSyncService.test.cpp`, round-trip + resolver cases in `ConfigManagerViews.test.cpp`, bucket-E `tests/ui/grid_parent_hierarchy.test.cpp` (Sort By toggles → view dirty + projection refresh).

## Deviations from plan

- **Issuelinks fallback site** (row 2): landed as a post-loop helper in `JiraIssueMappingPure.cpp` instead of inside `NormalizeParentRefObject` — the parser sees one field value at a time and never has the sibling `issuelinks` array; the mapper does. Same `"KEY - summary"` output.
- **Extra pure helpers**: `ParentKeyOf` and `PresentParentIds` were added beyond the planned list — both projection steps and the sync path needed them, and inlining would have tripped the dup gate.
- **Shared active-view resolver** (not in the plan): the sync worker's active-view loop was a 76-token clone of `AppController_Init.cpp`'s; the dup gate fired, so `ConfigManager::FindActiveViewOrFirst` now serves both (`ViewsStore` and `ViewWorkspaceState` share the `Views` / `ActiveViewId` shape).
- **`drawActiveProjectGridSort` decomposition** (not in the plan): the `H` / `G` fingerprint bits pushed it to 31 decision points (cap 30); fingerprint build + hierarchy-options snapshot moved to two static helpers.
- **Test-target registration**: each doctest target that links `ParentHierarchyPure.cpp` lists it once (explicit source lists, no glob) — a second entry produced duplicate-symbol link errors on the first attempt.
- **Bucket-E test is fixture-agnostic**: `basic-grid.json` has no parent / child pair, so the UI test asserts the toggle → dirty → projection-refresh contract rather than row order; row-order / depth semantics are pinned by bucket-A (`StoryGroupOrder`, `ComputeDepths`) instead. A fixture with a parent / child pair is the deferred-automation item (§ Verification (actual)).
- **Plan-lock path typo** fixed in the seeded lock (`refs/locks/parent-issue-hierarchy`) when the hook rejected the first write-set.
- **Visual-validation catch (post-PR)**: the toggles were a visible no-op in the running app. Root cause: the Jira mapper only stored `parent` when the active view carried it as a column (the selected-field loop), so the cache held zero parent values even though `parent` was always on the wire. `parent` is now mapped unconditionally after the loop, like `issuetype` / `comment`, ahead of the issuelinks fallback; two doctests pin the production shape. Bucket-A / bucket-E did not catch it because both feed `CachedTicket`s directly and never cross the mapper — the deferred parent/child bucket-E fixture should go through the deterministic Jira backend for that reason.
- **Visual-validation feedback (post-PR, user)**: (1) *Hide parent stories* now flattens the leaf rows — depth stays empty and the parent-id set is cleared after the filter step, so no indent and no tint survive even with *Story group* on; the guide's earlier claim that hide-parents re-added matched ancestors was wrong against the code and is corrected. (2) New global preference `TrackerConfig::LoadParentIssues` (default on; `load_parent_issues` / `config.set loadParentIssues`; Preferences → Editing → Grid behaviour) gates the worker-side parent fetch for every view — not in the plan, added on request.
- **`grill-with-docs`**: run at plan time ("triple check the plan"); no post-implementation re-grill — the deviations above are all mechanical (gate-driven) and change no contract.

## Verification (actual)

All run on 2026-09-07 in the session worktree, after the final lint-driven refactors (shared `FindActiveViewOrFirst`, `drawActiveProjectGridSort` decomposition).

- **Builds**: `ninja-iter-msvc` (GL standalone) rc=0; `SmatchetCore_DX12` dual-target rc=0; `ninja-test-msvc` rc=0; `ninja-ui-test-msvc` rc=0.
- **Bucket-A (doctest)**: `ctest --preset ninja-test-msvc` 7/7; filtered run `-tc="*ParentHierarchy*,*FindActiveViewOrFirst*,*HideParents*,*parent*"` 31 cases / 148 assertions, all pass.
- **Bucket-E (ImGui Test Engine)**: `UI_TEST_FILTER=GridParentHierarchy bash scripts/dev/test-ui-jira-deterministic-backend.sh` → `SortByToggles_ProjectionAndDirty` passed=1 failed=0.
- **Lint**: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` rc=0, all gates PASS (dup gate clean after the shared resolver; func caps clean after the fingerprint extraction). Advisory WARNs only: `tu-line-ceiling` on the pre-existing `SmatchetLocalization.cpp` whale, `comment-ratio` on two headers (unchanged ratios).
- **Docs**: `docs/guides/parent-hierarchy.md` + README § Features / § Documentation; `bash scripts/dev/test-docs.sh` (result recorded in the PR body).
- **Visual-validation pause**: exe launched from the iter build for the user's verdict (Sort By ↕ popup → Story group / Hide parent stories, indent, lavender tint).
- **`scripts/dev/test-all.sh`**: every gate through `test-lint-hook-split` passed; the run was stopped after hanging 12+ min inside `test-merge-gates.sh` (bats + `gh`, local env). Three pre-existing local-env failures, none touching this diff (no `.sh` / `.py` / `.bats` changed): `test-adapter-drift` (stale gitignored `.claude/agents` mirror), `archive_backlog_entry.bats` #16 and `gate_selftests.bats` #2 (both `--selftest` dogfoods, CRLF working copy).
- **Post-fix check**: doctest `-tc="*JiraSearchIssue*"` 19 cases / 52 assertions pass (2 new); live check after relaunch: 28 non-empty `parent` rows cached for Jira, every referenced parent present in the cache.
- **Adversarial pre-merge review (`code-review`, opus/high)**: 1 Critical / 4 High / 5 Medium / 3 Low. Fixed before merge: (Critical) the "Story group" -> "Parent group" rename only touched the loc-table row — `SmatchetLocalization::T(key, fallback)` returns the call-site FALLBACK for en-US, never `entry.English`, so the checkbox still rendered "Story group" and the bucket-E item ref never resolved, silently no-op'ing the feature's only UI-test coverage; fixed the fallback literal + tooltip prose. (High) `TicketSyncService::FetchMissingParentsIntoQueue` overwrote `summary.Warning` instead of appending, discarding a streamed-fetch warning (e.g. GitHub's page-cap notice); now appends with `; `. (High) the bucket-E hide-parents-while-story-group-on step asserted `cachedDepths.size() == ticketCount`, contradicting the code it claims to pin (hide-parents clears `cachedDepths`); fixed to assert empty. (High, perf) the quick-filter ancestor re-add called `ParentHierarchyPure::AncestorChain` per matched row, each rebuilding the id-index from scratch — O(n^2) on every keystroke with no view-size cap; added a public `ParentHierarchyPure::IdIndex` / `BuildIdIndex` + an index-taking `AncestorChain` overload, built once per filter-projection rebuild. Deferred as backlog (not blocking): uncancellable keyed parent fetch can block UI-thread shutdown/backend-switch joins on a large top-up; the fetch gates on the ACTIVE view while the grid reads hierarchy options per pane (ADR-0018); the fetch is uncapped and all-or-nothing per backend, so a failed/skipped top-up can let the stale-purge delete previously-cached parents; `issuelinks` is now requested on every Jira search page unconditionally. Six `docs/self-improvement/categories/{debt,tooling}/` entries filed 2026-09-07 for the deferred items plus two process gaps (the `T()` fallback trap; bucket-E item refs should hard-fail, not silently no-op).
- **Post-feedback check**: doctest filter `*parent*,*Parent*,*Config*,*PreferencesSchema*` 174 cases / 2085 assertions pass (round-trip, default-on, skip-fetch-when-off); lint gate all PASS; iter exe relaunched for the flat hide-parents view + the new preference.
- **Deferred automation**: a `tests/fixtures/jira_backend/` fixture with a parent / child pair so bucket-E can pin row order + depth + tint (today pinned only by bucket-A pure tests).
