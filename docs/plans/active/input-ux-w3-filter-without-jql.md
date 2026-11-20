# Plan — input UX Wave 3: filter without JQL

> **Slug**: `input-ux-w3-filter-without-jql` (matches this file's basename without `.md`).
>
> **Status**: `active` — plan only; drives seven PRs (S1 → S7) against `develop`. No plan-lock is claimed yet: each slice claims `refs/locks/input-ux-w3-filter-without-jql` from its own write set when its implementation starts (`bash agent-layer/agents/scripts/core/lock-claim.sh`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

[`docs/audits/INPUT_UX_AUDIT.md`](../../audits/INPUT_UX_AUDIT.md) § 5.3 lists "filter without JQL" as one Wave 3 structural item: suggestions and facet chips in the grid search box, and inline query errors (**#61**, **#63**, #68). § 7.6 has the evidence. Today the per-pane grid search box (`##GridFilter`, `DrawGridSearchBox` in `SmatchetGridHeaderUi.cpp`) sorts what is typed into a ticket key, a structured query or plain title text (`GridSearchInputClassifier.h`). It has no suggestions, no way to filter by status, assignee or label without writing a query, and no list of earlier queries. The only place with query autocomplete is the Views dashboard's Filter tab.

Two Wave 2 pieces already cover part of this, and this plan builds on them:

- **Piece 4.5, tolerant parsing** ([`docs/plans/input-ux-w2-tolerant-parsing.md`](../input-ux-w2-tolerant-parsing.md), merged in #2441 as `3d1ff166f`) added `JqlQueryValidate::ValidateQuery`, the `ParseMessage` type and the inline error widget (`Ui/SmatchetInlineError.h`). A malformed query typed in the grid box is now refused on Enter, but with a toast; the dashboard editor has the inline line. That plan's § Out of scope names "the inline error under the grid search box itself" as the follow-up. This plan is that follow-up.
- **Piece 4.7, one matcher** ([`docs/plans/input-ux-w2-one-matcher.md`](../input-ux-w2-one-matcher.md), merged in #2456) routed pickers, the grid title search and field-name suggestions through `TextMatchPure.h` (#62 and #67 are closed). Its debt entry `docs/self-improvement/categories/debt/2026-11-19-one-matcher-deferred.md` leaves a "showing close matches" cue under the grid search box unbuilt. That cue shares the line this plan adds, so this plan builds it (S7).

Three facts about the query layer shape the design (all verified in the source):

- **Jira, GitHub and Linear views are written in JQL.** Jira sends it as is. GitHub and Linear translate a safe subset at fetch time and drop the rest with a warning (`TranslateJqlToGitHubSearch` in `Tracker/GitHubQueryFromJql.h`, `TranslateJqlToLinearFilter` in `Tracker/LinearQueryFromJql.h`).
- **A Plane view query does not filter by field.** It carries only a project id and a key clause (`Tracker/PlaneProjectScope.h`), and the page fetch sends no status, assignee or label parameter (`PlaneIssueSearch.cpp`, `per_page` / `expand` / `cursor` / `sequence_id__in` / `updated_at__gte` only). The Tracker glossary says the same: the Plane filter mini-language "has no general evaluator".
- **A Linear pane is classified as Jira today.** `GridSearchBackend` has no `Linear` value, and `GridSearchBackendFromKey` sends "Linear" to `Jira`. So a Linear pane's query is validated against the app-wide Jira-style catalog, and `assignee = "Jane"` is rewritten to an account id even though Linear matches assignees by name.

After this lands, a user who knows no query language can filter a Jira, GitHub or Linear grid pane from its header. A **+ Filter** button adds a chip for status, assignee, label and a few more fields, picked from lists that work offline. Typing a field name in the search box of a Jira pane offers the field and its values. A malformed query shows what is wrong under the box while it is typed, and a query the tracker rejected, or partly ignored, says so in the same place. An empty focused box lists the last ten queries applied on that tracker, kept across restarts.

Closes (audit § 7.6): **#61**, **#63** (the rest: the line under the grid box, the funnel tooltip, the tracker's answer after sync), #68. Also closes the "close matches" item of the one-matcher debt entry. Not closed here: #64, #65, #70 (§ Out of scope).

## Approach

**Chips edit the view's query; the box keeps its three modes.** A chip row under the pane's toolbar shows the conditions of the pane view's query (`ViewDefinition::Jql`). Adding, changing or removing a chip rewrites that string with a new pure clause editor in the Tracker subsystem (`QueryFacetClausesPure`) and applies it through the existing `SmatchetUI::applyQueryToPaneView`. The change is therefore an unsaved edit with the Save / Discard strip, exactly like a query typed in the box. The box's own text is never touched by a chip, so a title search still narrows the rows the chips fetched. The trade-off: one string of record and no new view storage shape, at the price that a chip can only say what the pane's backend can evaluate. The GitHub label chip is single-value, and a Plane pane gets no chips (the button is drawn disabled and its tooltip says why).

**Suggestions reuse the dashboard's autocomplete shell, and never take a title search.** The box moves to `SmatchetInput::TextBuf` with the existing `TrackerQueryAcp_InputTextCallback`. A pure gate (`ShouldOfferQuerySuggestions`) allows the list only on a Jira pane and only when the text is query-like: it already classifies as a query, the caret follows a field and an operator, or the whole text is one word of two or more letters that starts a field name. No row is highlighted until the user presses Down, and a mouse hover does not arm Enter, so a bare Enter keeps today's route (title filter, key jump or query apply). Autocomplete state cannot live on `GridPane`, which is copied by value while `JqlEditorState` owns two `std::future`s. One session-level `JqlEditorState gridSearchAcp` serves whichever pane's box has the caret and is reset when that pane or its tracker changes, so the user names it resolved always belong to the pane being queried. Recent queries are rows of the same popup, shown when the box is empty and focused, which gives them Up / Down / Enter / click for free. They are stored in the shared recents store (`RecentSelections.h`), one list per tracker instance.

**One line under the box says what is wrong.** The box validates its text only when the text changes, and one pure function (`PickGridSearchLine`) picks what to show, in this order: a blocking parse error, the tracker's rejection of the last applied query, a warning (an unknown field, or clauses GitHub or Linear dropped), then the "close matches" hint. The line is drawn with the existing inline error widget as an overlay, so the toolbar does not move, and the funnel glyph's tooltip repeats it. The tracker's answer comes from the last sync session's stats: a small Sync change adds the warning text and the transient flag the session already computes, and the box reads them once the session it started has finished.

## Interface contracts

No change to `ITrackerBackend` or its six role interfaces, to view storage, the SQLite cache, MCP tool schemas or sol2 bindings. Dialect knowledge stays in one Tracker pure TU; the Ui maps a pane's backend key to a dialect.

```cpp
// Source/Core/include/Tracker/QueryFacetClausesPure.h (new; .cpp beside it) — no I/O, no ImGui, C++14
namespace query_facets {
enum class FacetDialect : unsigned char { Jql, GitHubSubset, LinearSubset };   // Plane has no dialect
enum class FacetOp : unsigned char { Eq, NotEq, In, NotIn };
struct FacetClause {
    std::string FieldId; FacetOp Op = FacetOp::Eq; std::vector<std::string> Values;
    bool CurrentUser = false;            // the value is currentUser()
    std::size_t At = 0, Len = 0;         // byte span in the parsed query
};
struct FacetParse {
    bool Chippable = false;              // top level is AND-only; see the round-trip rule below
    std::vector<FacetClause> Clauses;    // clauses on a facet field
    std::vector<std::pair<std::size_t, std::size_t>> OtherSpans;   // every other clause, byte-for-byte
    std::size_t OrderByAt = std::string::npos;
};
FacetParse ParseFacetClauses(const std::string& query, FacetDialect d, const std::vector<std::string>& facetFieldIds);
// Replaces the first Eq / In clause on the same field, else appends before ORDER BY. Values are quoted with
// tracker_query_suggest::QueryQuotedValue. A query that is not chippable is wrapped: "(<query>) AND <clause>".
std::string SetFacetClause(const std::string& query, FacetDialect d, const FacetClause& clause);
std::string RemoveFacetClause(const std::string& query, std::size_t at, std::size_t len);   // drops the clause and one adjacent AND
bool DialectSupports(FacetDialect d, const std::string& fieldId, FacetOp op, std::size_t valueCount);
// The facet field ids a dialect can filter by, in menu order.
const std::vector<std::string>& FacetFieldIds(FacetDialect d);
}
```

**Round-trip rule.** A query is `Chippable` when its top level is joined by `AND` only, it has no top-level `OR` or `NOT`, its only parentheses are `IN` lists, and every clause is `<field> (= | != | IN | NOT IN) <value | currentUser() | (list)>`. A trailing `ORDER BY …` is kept verbatim. In the GitHub dialect a bare `word:value` token (`type:pr`, which the translator accepts beside JQL clauses) is one more clause, kept as it is. Then:

- a clause on a facet field is a chip that can be changed or removed. A Jira assignee chip shows the person's name, resolved from the saved roster the way `jql_user_display::RenderQueryWithUserNames` does, and falls back to the id;
- every other clause stays byte-for-byte and shows as one read-only chip, "N more conditions", with the text in its tooltip;
- a query that is not chippable shows one read-only chip, "Custom query". On Jira, + Filter still works by wrapping the query. On GitHub and Linear it is disabled with the reason, because their translators drop parentheses.

**Per backend:**

| | Jira | Plane | GitHub | Linear |
|---|---|---|---|---|
| View query | JQL | project id + key clause only | JQL subset, translated | JQL subset, translated |
| Typed suggestions | `BuildJqlSuggestions` (existing) | none | none in this plan | none in this plan |
| Status chip | `status = "X"` / `status in (…)`, by name | none | `status = "Open"` or `"Closed"`, one value | `status in (…)` |
| Assignee chip | `assignee = "<accountId>"` / `currentUser()` | none | `assignee = "<login>"` / `currentUser()`, one value, no `!=` | `assignee = "<name>"` / `currentUser()`, one value |
| Label chip | `labels in (…)` | none | `labels = "x"`, one per chip; several chips are AND | `labels in (…)` |
| Other facets | priority, issuetype, components, fixVersion, resolution, reporter | none | reporter | priority, project, team |
| Validation | `QueryDialect::Jql` + the pane's catalog | `QueryDialect::FieldValue` | `QueryDialect::FieldValue` | `QueryDialect::Jql`, structure only (no catalog: the validator's built-in field names are Jira's and do not include `team`) |
| Offline | lists from the cached catalog and saved roster; live user search paused | recents only | cached lists | cached lists |

**Parse messages** (`Tracker/TrackerFieldValueParseExplain.h`): two ids appended after `QueryUnknownField`: `QueryRejectedByTracker` (Arg: the tracker's text; sentence "The tracker did not run this query: …", which stays true for a sign-in failure) and `QueryClausesDropped` (Arg: the translator's warning). Text in `DescribeParseMessage`. Neither blocks; `IsBlockingQueryError` is unchanged.

**Sync** (`Sync/SyncSessionStats.h`): `SyncSessionStats` gains five fields beside `Error`: `std::string Warning;`, `bool ErrorTransient = false;`, `bool Cancelled = false;` (set where `RecordSessionStats` today substitutes "cancelled or superseded before a result"), and `std::string ViewId;` / `std::string Query;` (the view the session fetched and its query, captured in `StartStreamingSync` next to `sessionViewId_`). `TicketSyncService::RecordSessionStats(const std::string& fetchError, bool fetchErrorTransient, const std::string& fetchWarning, bool fullSyncCompleted)` widens from the two-argument form; its one call site, in `FinalizeStreamingSessionIfDone`, already holds all three values as locals. The stats are per tracker context: `AppController::GetLastSyncSessionStats` returns the focused context's. The `sync.stats` command's JSON is unchanged.

```cpp
// Source/Core/include/Ui/GridSearchBoxPure.h (new, header-only) — no ImGui, C++14
namespace smatchet { namespace gridsearch {
// True only for a Jira pane whose text is query-like (see § Approach). `fields` may be null (no suggestions).
bool ShouldOfferQuerySuggestions(const char* text, int cursor, GridSearchBackend b, const std::vector<TrackerField>* fields);
enum class GridSearchLineKind : unsigned char { None, BlockingError, TrackerRejection, Warning, Hint };
struct GridSearchLine { GridSearchLineKind Kind = GridSearchLineKind::None; TrackerParseExplain::ParseMessage Msg; };
GridSearchLine PickGridSearchLine(const TrackerParseExplain::ParseMessage& typed,
                                  const TrackerParseExplain::ParseMessage& rejection, bool closeMatches);
struct QueryRejectWatch { bool Armed = false; std::uint64_t BaselineRequestId = 0; std::string ContextKey;
                          std::string ViewId; std::string Query; double NextPollAt = 0; };
// Resolves only on a finished session newer than the baseline whose ViewId and Query equal the watch's (any other
// session leaves it armed). A non-transient Error gives QueryRejectedByTracker, a Warning gives QueryClausesDropped;
// a transient Error, a cancelled session or a clean one gives None. Sets `outResolved` and disarms.
TrackerParseExplain::ParseMessage ResolveQueryRejectWatch(QueryRejectWatch& w, const smatchet::sync::SyncSessionStats& s,
                                                          bool& outResolved);
}}
```

**Classifier** (`Ui/GridSearchInputClassifier.h`): `GridSearchBackend` gains `Linear`; `GridSearchBackendFromKey("Linear")` returns it; `LooksLikeTicketKey` treats a Linear key like a Jira key (the same `TEAM-123` shape, so today's behaviour is kept). The account-id rewrite in `applyGridSearchEnter` stays Jira-only, which stops rewriting a Linear assignee name into an id.

**Autocomplete shell** (`Ui/SmatchetAutocompleteUi.h`, `QuerySuggestTypes.h`, `Ui/SmatchetInput.h`):

- `TrackerQueryAcpCallbackUserData` gains `bool suggestEnabled = true;`, `bool hoverArmsEnter = true;` and `QuerySuggestMemo* memo = nullptr;` (a memo of buffer length and hash, caret, selection and catalog pointer; an unchanged key skips the suggestion build).
- `TrackerQueryAcp_DrawPopup` and `TrackerQueryAcp_TickDebouncedUserSearch` take the `TrackerQuerySuggestKind` (and the tick a `bool trackerOffline`) as parameters. Today both read the global `d.cfg.TrackerType`, which is wrong for a pane on another backend, and the user search has no offline pause.
- `QuerySuggestion` gains `bool CommitOnPick = false;` and `JqlEditorState` gains `bool jqlAcpCommitAfterReplace = false;`: picking a recent-query row replaces the whole buffer and then applies it.
- `SmatchetInput::TextOptions` gains `ImGuiInputTextCallback callback = nullptr; void* callbackUserData = nullptr;`, passed through by `TextBuf`.

**State.** `GridPane` (copyable, not persisted) gains: the last validated text with its classified kind and `ParseMessage` (`gridSearchCheckedText`, `gridSearchTyped`); `gridSearchRejection` and `QueryRejectWatch gridSearchRejectWatch`; the facet memo (`facetSource`, `facetParse`, re-parsed only when the view's query changes); `bool gridFilterCloseMatches`; `int gridFacetMenuFramesLeft`. `UiDrawSession` gains `JqlEditorState gridSearchAcp` with its owner (`gridSearchAcpPaneId`, `gridSearchAcpCacheKey`) and `PaneDeferredActionKind::GridFacetApply`, whose payload reuses `paneDeferredSearchText`. `DrainUiDrawSessionFuturesBeforeAppTeardown` also waits on `gridSearchAcp`'s two futures.

**Recent queries.** Same store, config key and JSON shape as `smatchet::recents`: the `recent_selections` array in the config file (`Source/Core/src/RecentSelections.cpp:16`, `ToJson` `:66`, `FromJson` `:77`, the queued save `:170-181`). No SQLite table and no migration. Scope: `MakeScopeKey(app.PaneCacheBackendKey(pane.id), "query.grid")`. A query scope keeps 10 entries of up to 512 bytes (the box's buffer size); every other scope keeps 5 of 256 as today. The limits come from one internal `LimitsForScope(scope)` that `Record` and `FromJson` both apply. A query is recorded only when a typed query was applied (`ApplyQueryResult::Ok` in `applyGridSearchEnter`), in the form the user typed. A chip change and the User Info "add to query" action do not record. An older build reading the file trims the lists to 5 entries.

**Command.** One new registry entry, `grid.search.add_filter` ("Open the focused pane's + Filter menu"), shaped like `grid.search.focus`: it sets `gridFacetMenuFramesLeft` on the focused pane. No default key. It is reachable from the CLI, the palette, MCP and Lua through the registry, with no schema of its own.

**Plumbing, hop by hop:**

| Flow | Hops |
|---|---|
| Keystroke → list → accept | `DrawGridSearchBox` → `SmatchetInput::TextBuf(label, pane.gridSearchBuf, kGridSearchBufSize, opts)` with `opts.callback = TrackerQueryAcp_InputTextCallback` → `HandleAcpAlwaysEvent` (returns early when `!suggestEnabled` or the memo matches) → `RunSuggestBuild(kind, …, *fields, *users, …)` → `TrackerQueryAcp_DrawPopup(d, d.gridSearchAcp, rectMin, rectSize, build, merged, kind)` → a pick is queued by `TrackerQueryAcp_QueueApplyReplacement` and applied by the next frame's callback. `fields` is `app.GetPaneAvailableFields(pane.id)`, held in a local `shared_ptr` for the frame. |
| Enter | The callback sets `gridSearchAcp.jqlWantsApplyFromEnter` only when no row is highlighted. The box latches `PaneDeferredActionKind::GridSearchCommit` from that flag, not from `TextResult::enterCommitted`, because Enter on a highlighted row must not apply. The replay in `drawGridPaneWindows` calls `applyGridSearchEnter`, which on Jira maps names to account ids with `TrackerQueryAcp_QueryWithAccountIds(*paneFields, users, d.gridSearchAcp, input)` when `d.gridSearchAcpPaneId == target.id`, else with the catalog-only `RenderQueryWithAccountIds` it uses today. |
| Chip pick → apply | Chip or + Filter popup (drawn inside the pane) → `query_facets::SetFacetClause(view.Jql, dialect, clause)` → latch `GridFacetApply` with `paneDeferredSearchText = newQuery` → replay beside the search commit → `applyQueryToPaneView(app, d, *target, query)`. A multi-select picker applies once, when its popup closes. |
| Validate → line | Each frame compares the buffer with `pane.gridSearchCheckedText`. Only on a change: `ClassifyGridSearchInput` and `JqlQueryValidate::ValidateQuery(text, dialect, paneFields.get())` → `pane.gridSearchTyped` → `PickGridSearchLine(…)` → `DrawInlineParseErrorBelowItem(msg)` (a hint through `DrawInlineParseHintBelowRect`). The funnel tooltip appends `DescribeParseMessage(line.Msg)`. The Enter refusal keeps its early return in `applyGridSearchEnter`; its toast is removed because the line is already showing. |
| Tracker's answer → line | A successful apply (typed or chip) arms `pane.gridSearchRejectWatch` after `applyQueryToPaneView` returns `Ok` (the pane's view is then the active one) with `BaselineRequestId = app.GetLastSyncSessionStats().RequestId`, `ContextKey = app.PaneCacheBackendKey(target.id)`, the view id and the query as applied. While it is armed and the pane is focused, at most every 250 ms: when `app.FocusedCacheBackendKey()` equals `ContextKey`, `ResolveQueryRejectWatch(w, app.GetLastSyncSessionStats(), resolved)` → `pane.gridSearchRejection`. Matching on the view and the query, not only on the request id, keeps an older session that finishes late from being read as this query's answer. The next edit of the box, the next apply and a view switch clear it. |
| Applied → recents → list | `applyGridSearchEnter` on `Ok` → `smatchet::recents::RecordAndPersist(scope, input)` (the config-save worker writes; the frame never waits). An empty focused box reads `SharedGet(scope)` once per focus gain → rows `{Label = q, Insert = q, CommitOnPick = true}` replacing the whole buffer. |

## Files to modify

Grouped by slice (§ Implementation handoff). New files are marked; a synonym grep for `QueryChip|QueryFacet|FacetChip|RecentQuer|GridSearchSuggest|GridSearchAcp|QueryClauseEdit` over `Source/` and `tests/` returns nothing.

**S1 — Tracker: the clause editor and two parse messages**

1. `Source/Core/include/Tracker/QueryFacetClausesPure.h`, `Source/Core/src/Tracker/QueryFacetClausesPure.cpp` (new): § Interface contracts. Includes `TrackerQuerySuggestCommon.h` only; no cpr, SQLite, ImGui or `Ui/` header.
2. `Source/Core/include/Tracker/TrackerFieldValueParseExplain.h`, `Source/Core/src/Tracker/TrackerFieldValueParseExplainText.cpp`: the two `ParseMessageId` values and their sentences.
3. `Source/Core/src/SmatchetLocalization.cpp`: the two `parse.*` rows with French rows.
4. `Source/Core/src/Tracker/CONTEXT.md`: glossary terms **Facet clause** and **Chippable query**; one line each stating that a Plane view query filters by key only and that GitHub and Linear view queries are a translated JQL subset.
5. `tests/Core/QueryFacetClausesPure.test.cpp` (new), `tests/CMakeLists.txt`: bucket A, added to `add_executable(SmatchetTests …)` and the Linux subset.

**S2 — Sync: session stats say how the session ended and for which query**

6. `Source/Core/include/Sync/SyncSessionStats.h`, `Source/Core/include/Sync/TicketSyncService.h`, `Source/Core/src/Sync/TicketSyncService.cpp`: § Interface contracts.
7. `tests/Core/TicketSyncService.test.cpp`: stats carry `Warning`, `ErrorTransient`, `Cancelled`, `ViewId` and `Query`; the error-only path is unchanged.

**S3 — Ui: the line under the box**

8. `Source/Core/include/Ui/GridSearchInputClassifier.h`: `GridSearchBackend::Linear`.
9. `Source/Core/include/Ui/GridSearchBoxPure.h` (new): `PickGridSearchLine`, `QueryRejectWatch`, `ResolveQueryRejectWatch` (the suggestion gate arrives in S5).
10. `Source/Core/include/GridPane.h`: the validation memo, the rejection and its watch.
11. `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp`: validate on change, draw the line, extend the funnel tooltip, poll the watch; the inline Esc block becomes `SmatchetInput::ClearSearchOnEscape` (closes debt `2026-11-18-grid-search-esc-onto-shared-helper`, including the deviation marker in `SmatchetInput.cpp`).
12. `Source/Core/src/Ui/SmatchetGridSearchUi.cpp`: dialect and catalog per pane (`GetPaneAvailableFields`), no account-id rewrite on Linear, arm the watch on `Ok`, drop the refusal toast.
13. `Source/Core/src/SmatchetLocalization.cpp`: remove `gridsearch.query_refused` if nothing else reads it.
14. `tests/Core/GridSearchBoxPure.test.cpp` (new), `tests/Core/GridSearchInputClassifier.test.cpp`, `tests/CMakeLists.txt`, `tests/ui/tolerant_parse_inline_error.test.cpp`, `tests/ui/grid_search_apply.test.cpp`.

**S4 — Ui: the autocomplete shell takes a second host**

15. `Source/Core/include/Ui/SmatchetInput.h`, `Source/Core/src/Ui/SmatchetInput.cpp`: the callback pass-through.
16. `Source/Core/include/QuerySuggestTypes.h`: `QuerySuggestion::CommitOnPick`, `QuerySuggestMemo`.
17. `Source/Core/include/Ui/SmatchetAutocompleteUi.h`, `Source/Core/src/Ui/SmatchetAutocompleteUi.cpp`, `Source/Core/include/Ui/SmatchetUiSession.h` (`jqlAcpCommitAfterReplace`): § Interface contracts.
18. `Source/Core/src/Ui/SmatchetViewsDashboardUi_widgets.cpp`: the dashboard call site passes the kind and the offline flag. Behaviour there changes in one way only: the user search pauses while the tracker is offline.
19. `tests/Core/AutocompleteListNav.test.cpp`: `CommitOnPick`, the memo skip, hover not arming Enter.

**S5 — Ui: suggestions and recent queries in the box**

20. `Source/Core/include/Ui/GridSearchBoxPure.h`: `ShouldOfferQuerySuggestions`.
21. `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp`: the box becomes `SmatchetInput::TextBuf` hosting the shell; the recents rows; the placeholder gains an example (`status = Done`).
22. `Source/Core/include/Ui/SmatchetUiSession.h`, `Source/Core/src/Ui/SmatchetUI.cpp`: `gridSearchAcp` with its owner, and the teardown drain.
23. `Source/Core/src/Ui/SmatchetGridSearchUi.cpp`: the Enter mapping above; record the recent query.
24. `Source/Core/include/RecentSelections.h`, `Source/Core/src/RecentSelections.cpp`: `LimitsForScope` and the query scope.
25. `Source/Core/src/Ui/SmatchetViewsDashboardUi_widgets.cpp`: the dashboard editor lists the same recent queries when it is empty and focused. A pick fills the editor and does not apply (Enter there saves the view; #64).
26. `Source/Core/src/SmatchetLocalization.cpp`, `docs/guides/keyboard-shortcuts.md`: the hint, the "Recent queries" header, the keys.
27. `tests/Core/GridSearchBoxPure.test.cpp`, `tests/Core/RecentSelections.test.cpp`, `tests/ui/grid_search_apply.test.cpp`.

**S6 — Ui + Commands: facet chips**

28. `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp`: `DrawGridFacetChips` (new) and the + Filter menu, through `smatchet::dropdown::DrawBody`.
29. `Source/Core/include/GridPane.h`, `Source/Core/include/Ui/SmatchetUiSession.h`, `Source/Core/src/Ui/SmatchetGridPaneWindows.cpp`: the facet memo, `GridFacetApply` and its replay.
30. `Source/Core/src/Commands/Builtin/BuiltinCommands_Ui.cpp`: `grid.search.add_filter`.
31. `Source/Core/src/SmatchetLocalization.cpp`, `docs/guides/keyboard-shortcuts.md`, `docs/guides/cli.md` (the command list, if the registry-parity gate asks for it).
32. `tests/ui/grid_facet_chips.test.cpp` (new), `tests/ui/CMakeLists.txt` (`_SMATCHET_UI_TEST_SOURCES`).

**S7 — Ui: the "close matches" hint**

33. `Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp`: `ApplyGridFilterProjection` sets `pane.gridFilterCloseMatches` when the letters-in-order fallback produced the rows.
34. `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp`, `Source/Core/src/SmatchetLocalization.cpp`: the hint through `PickGridSearchLine`.
35. `docs/self-improvement/categories/debt/2026-11-19-one-matcher-deferred.md`: strike the close-matches item.
36. `tests/ui/grid_search_apply.test.cpp`.

CMake: Core `.cpp` files are globbed into `CORE_SOURCES`; the two new doctest files are listed in `tests/CMakeLists.txt`, the new UI test in `tests/ui/CMakeLists.txt`. No new library target.

### File-level changes

Existing symbols the slices change or call (one per row, for `plan-doc-table-probe.sh`):

| File | Symbol | CMake variable |
|---|---|---|
| `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp` | `DrawGridSearchBox` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp` | `DrawHeaderViewToolbar` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetGridSearchUi.cpp` | `applyGridSearchEnter` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetGridSearchUi.cpp` | `applyQueryToPaneView` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetGridPaneWindows.cpp` | `GridSearchCommit` | `CORE_SOURCES` |
| `Source/Core/include/Ui/GridSearchInputClassifier.h` | `GridSearchBackendFromKey` | `CORE_SOURCES` |
| `Source/Core/include/Ui/GridSearchInputClassifier.h` | `LooksLikeTicketKey` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetAutocompleteUi.cpp` | `HandleAcpAlwaysEvent` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetAutocompleteUi.cpp` | `RunSuggestBuild` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetAutocompleteUi.cpp` | `TrackerQueryAcp_DrawPopup` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetAutocompleteUi.cpp` | `TrackerQueryAcp_TickDebouncedUserSearch` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetAutocompleteUi_detail.cpp` | `ResolveAcpCommit` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetViewsDashboardUi_widgets.cpp` | `TrackerQueryAcpCallbackUserData` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetInput.cpp` | `ClearSearchOnEscape` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetInput.cpp` | `TextBuf` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetUI.cpp` | `DrainUiDrawSessionFuturesBeforeAppTeardown` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp` | `ApplyGridFilterProjection` | `CORE_SOURCES` |
| `Source/Core/src/Sync/TicketSyncService.cpp` | `RecordSessionStats` | `CORE_SOURCES` |
| `Source/Core/src/Sync/TicketSyncService.cpp` | `FinalizeStreamingSessionIfDone` | `CORE_SOURCES` |
| `Source/Core/src/Sync/TicketSyncService.cpp` | `StartStreamingSync` | `CORE_SOURCES` |
| `Source/Core/src/Sync/TicketSyncService.cpp` | `FetchErrorTransient` | `CORE_SOURCES` |
| `Source/Core/src/Tracker/TrackerFieldValueParseExplainText.cpp` | `DescribeParseMessage` | `CORE_SOURCES` |
| `Source/Core/src/Tracker/JqlQueryValidatePure.cpp` | `IsBlockingQueryError` | `CORE_SOURCES` |
| `Source/Core/src/RecentSelections.cpp` | `RecordAndPersist` | `CORE_SOURCES` |
| `Source/Core/src/Commands/Builtin/BuiltinCommands_Ui.cpp` | `grid.search.focus` | `CORE_SOURCES` |

## Existing utilities reused

- `JqlQueryValidate::ValidateQuery` / `IsBlockingQueryError`, `Source/Core/include/Tracker/JqlQueryValidatePure.h:23-28`: the structural check; this plan only changes where its result is shown.
- `DrawInlineParseErrorBelowItem` / `DrawInlineParseHintBelowRect` / `InlineErrorTextForTests`, `Source/Core/include/Ui/SmatchetInlineError.h:31,39,50`: the one widget that draws a parse message under a field.
- `TrackerQueryAcp_InputTextCallback`, `TrackerQueryAcp_DrawPopup`, `TrackerQueryAcp_QueueApplyReplacement`, `TrackerQueryAcp_QueryWithAccountIds`, `Source/Core/include/Ui/SmatchetAutocompleteUi.h:35-70`: the suggestion list, its keys and the name-to-id mapping.
- `BuildJqlSuggestions`, `Source/Core/include/Tracker/JqlSuggestEngine.h:11`, and `tracker_query_suggest::QueryQuotedValue` / `FindTrackerField`, `Source/Core/include/Tracker/TrackerQuerySuggestCommon.h:27,50`.
- `SmatchetInput::TextBuf` / `ClearSearchOnEscape`, `Source/Core/include/Ui/SmatchetInput.h:81,117`.
- `smatchet::dropdown::DrawBody` / `SingleSelect` / `TickBoxes`, `Source/Core/include/FilterDropdown.h:71-81`: every chip's value picker, with the shared matcher and per-field recent picks (scope `MakeScopeKey(paneCacheKey, fieldId)`, the one the cell pickers use).
- `smatchet::recents::RecordAndPersist` / `SharedGet` / `MakeScopeKey`, `Source/Core/include/RecentSelections.h`: memory reads per frame, saves on the config-save worker.
- `AppController::GetPaneAvailableFields` / `PaneCacheBackendKey` / `FocusedCacheBackendKey` / `GetLastSyncSessionStats`, `Source/Core/include/AppController.h:848,1086,1088,1588`.
- `DataFreshnessCue`, `Source/Core/include/DataFreshnessCue.h`: the cue beside a chip picker's cached list.
- `SmatchetUI::applyQueryToPaneView`, `Source/Core/src/Ui/SmatchetGridSearchUi.cpp:160`: the one unsaved-edit apply path.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — nothing is extracted or split. `SmatchetGridHeaderUi.cpp` is 786 lines and gains about 250 across S3 to S7, which stays under the 1,200-line advisory ceiling; `DrawGridFacetChips` and the box each stay under the 200-line draw cap as separate functions.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: the box and the chip row draw for every pane every frame. An empty box runs no classifier, no validation and no callback. Validation, the facet parse and the recents read are memoized on a text, query or focus change. A pane whose view has no conditions draws one disabled-looking + Filter button and no chip row.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no new I/O on the UI thread. The recents save is the existing config-save worker; the Jira user search is the existing debounced worker future, and now also pauses while offline.
- **Pillar 3 (never crash)**: `gridSearchAcp`'s futures are drained at teardown with the other session futures. Clause spans are bounds-checked before a rewrite; a span that no longer fits the query (the view changed under the popup) makes the edit a no-op. A view apply stays behind the deferral latch, never mid-table.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: + Filter is a command and a focusable button; chips are focusable items (Enter opens, Delete removes); the line and the chips use existing theme colours, no new palette token.
- **Pillar 6 (offline-first)**: chip lists come from the pane's cached catalog, the saved user roster and the loaded rows, with a freshness cue when the tracker is offline; "No options" is shown only for a list the tracker answered. No request is sent while offline. A transient sync failure is never shown as "the tracker rejected the query".

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

This PR is docs-only. The gates below bind the implementation slices.

1. **PR-fast CI** — fires for S3, S5, S6, S7: `scripts/dev/perf-scenario-map.json` maps `Source/Core/src/Ui/SmatchetGrid*.cpp` to `priority-grid-scroll` and `cell-edit-burst`; `side-by-side-2-grid` (also in `perf-pr-fast-set.json`) is the one that draws two headers and two chip rows. S1, S2 and S4 fall back to `idle`.
2. **Pillar 2 static scanner** — N/A: no new sync I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` is not touched.
4. **Visible-cue bucket-E harness** — N/A: no new stall path. The user search keeps its "Searching…" row.
5. **Marker inventory** — fires for S5: two permanent markers, `SMATCHET_UI_PERF_SCOPE("grid.search.box")` (once per pane per frame) and `"grid.search.suggest"` (around the suggestion build, active box only); regenerate `docs/perf/MARKER_INVENTORY.md` in that PR.

**Pre-push local check**: `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against `side-by-side-2-grid` and `priority-grid-scroll` for S3, S5, S6 and S7.

**Override**: none planned.

## Risks / non-goals

- **A suggestion takes over a title search.** Mitigated by the gate, by no pre-highlighted row and by hover not arming Enter; pinned by the bucket-E case "typing `sta` offers `status`, Enter still filters titles".
- **A chip writes a clause the backend ignores.** `DialectSupports` is the only way a picker is offered, and its table is tested against the two translators' own doctest expectations. A drift shows as the `QueryClausesDropped` line after the next sync.
- **A chip rewrite damages a hand-written query.** Only chippable queries are edited in place; everything else is kept byte-for-byte or wrapped whole. The bucket-A table asserts parse → set → remove returns the original bytes.
- **The tracker's answer is attributed to the wrong query.** The watch records the request id, the cache key, the view id and the query at apply time, resolves only on a session that matches all of them, and is polled only on the focused pane; a newer apply or an edit clears it. A session that never finishes, or is cancelled, leaves the line empty, not wrong.
- **A non-transient failure that is not about the query** (sign-in, a worker fault) also lands on the line. Its wording does not claim a syntax fault, and the tracker's own text follows it.
- **Backend leakage**: none. No role-interface virtual is added; the Ui names a dialect, not a client.
- **Save format**: the recents JSON shape is unchanged; an older build trims query lists to 5 entries. View storage, config keys and the SQLite cache are untouched.
- **Recent queries hold what the user typed**, which can include names. They sit in the same config file as the saved views that hold the same text.
- **Dual-target**: everything is C++14 and compiles in `SmatchetCore_DX12`; no GLFW or OpenGL include; no `SMATCHET_WITH_*` gating changes.
- **Localization churn**: about 20 new keys. `SmatchetLocalization.cpp` is also edited by the sibling Wave 3 plans, so expect textual rebases.
- **Shared popup id**: the suggestion window name is shared with the dashboard editor. Only one input holds the caret at a time; a UI test keys on the owning input, not the popup name.
- **File overlap with open work**: #2452 edits `GridPane.h`, `SmatchetUiSession.h`, `SmatchetUI.cpp` and `SmatchetActiveProjectGridTable.cpp`; #2439 edits `SmatchetUI.cpp`; the sibling Wave 3 plan "the grid as a spreadsheet" (slug `input-ux-w3-grid-spreadsheet`, still in review as #2461, so not linked here) edits `SmatchetActiveProjectGridTable.cpp`. § Implementation handoff orders the slices behind them.
- **No `std::regex`**: the clause parser is a hand-written scanner, like the two translators.
- **Non-goals**: a general JQL parser; chips for custom fields (`cf[…]`), dates or numbers; OR or grouping in chips; a Plane filter; typed suggestions on GitHub and Linear; typo tolerance for clause values.

## Implementation handoff

No slice waits on #2441: it merged on 2026-11-20 and its API is on `develop`. S1, S2 and S4 can start at once and in parallel. S3, S5, S6 and S7 all write `SmatchetGridHeaderUi.cpp` and run in that order. Each slice is one PR, claims the plan-lock for its write set when it starts, and appends to § Implementation log.

**S1 — clause editor and parse messages**
- **Owner subsystem**: `Source/Core/src/Tracker/` (strict lint zone).
- **Write set**: § Files to modify items 1-5.
- **Pre-resolved decisions**: a pure helper, not a role-interface method (the Tracker leaf's "place the change" rule); there is no Plane dialect; an edit replaces the first `=` / `IN` clause on the same field; `ORDER BY` is kept verbatim; a non-chippable query is wrapped, never re-parsed.
- **Inline inventory**: `Source/Core/src/Tracker/GitHubQueryFromJql.cpp` and `Source/Core/src/Tracker/LinearQueryFromJql.cpp` hold the supported-clause lists that `DialectSupports` mirrors (their `SplitTopLevelAnd` is file-private: mirror it, do not export it); `Source/Core/include/Tracker/TrackerFieldValueParseExplain.h:56` (`QueryUnknownField`, the append point).
- **Order**: first; S3 and S6 need it.

**S2 — session stats**
- **Owner subsystem**: `Source/Core/src/Sync/` (strict lint zone).
- **Write set**: items 6-7.
- **Pre-resolved decisions**: `Warning` and `ErrorTransient` mirror the locals `FinalizeStreamingSessionIfDone` already reads under `QueueMutex`; `ViewId` and `Query` are captured on the UI thread in `StartStreamingSync`; no new lock, no new thread hop; the `sync.stats` JSON is unchanged. The streaming session is the only path that ends a view fetch (`ApplyIssueFetchPack` has no caller under `Source/Core/src`), so one recording site is enough.
- **Inline inventory**: `Source/Core/src/Sync/TicketSyncService.cpp:676-683` (the three locals), `:732` (the call), `:759` (the definition), `:771-777` (the substituted "cancelled or superseded" text), `:1186-1192` (`StartStreamingSync`, where the session's view is resolved); `Source/Core/include/Sync/SyncSessionStats.h:47` (`Error`).
- **Order**: any time before S3.

**S3 — the line under the box**
- **Owner subsystem**: `Source/Core/src/Ui/` (grid).
- **Write set**: items 8-14.
- **Pre-resolved decisions**: the line replaces the Enter toast; validation runs on a text change, never per frame; the watch is polled on the focused pane only, at most every 250 ms; a transient failure shows nothing here (the connectivity toast already covers it); no account-id rewrite on Linear; a Linear pane is validated for structure only; this-frame focus gates the poll (Ui leaf § Frame state and focus).
- **Inline inventory**: `Source/Core/src/Ui/SmatchetGridSearchUi.cpp:105-116` (validate and refuse), `:128-131` (the rewrite); `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp:288-294` (glyph and tooltip), `:308-319` (the inline Esc block).
- **Order**: after S1 and S2, and after #2452 merges (both edit `GridPane.h`).

**S4 — the shell takes a second host**
- **Owner subsystem**: `Source/Core/src/Ui/`.
- **Write set**: items 15-19.
- **Pre-resolved decisions**: no behaviour change in the dashboard editor except the offline pause; the kind is a parameter, never read from `d.cfg`; `QuerySuggestion` stays a C++14 aggregate.
- **Inline inventory**: `Source/Core/src/Ui/SmatchetAutocompleteUi.cpp:106` (`RunSuggestBuild`), `:214` (`HandleAcpAlwaysEvent`), `:303` and `:541` (the two `d.cfg.TrackerType` reads), `:346` (hover sets the selection); `Source/Core/src/Ui/SmatchetViewsDashboardUi_widgets.cpp:311` (the one call site); `Source/Core/src/Ui/SmatchetInput.cpp:157` (`TextBuf`).
- **Order**: independent; before S5.

**S5 — suggestions and recent queries**
- **Owner subsystem**: `Source/Core/src/Ui/` (grid). `RecentSelections.{h,cpp}` sits at the include root with no leaf; it rides with this slice as the store's only new caller.
- **Write set**: items 20-27.
- **Pre-resolved decisions**: one session-level `gridSearchAcp`, reset when the owning pane or its cache key changes; suggestions on Jira only; no pre-highlighted row and no hover-armed Enter; Esc closes a visible list first and clears the box second; Tab accepts the first row only while a list is showing; a recent row applies on pick in the grid box and only fills the dashboard editor; the `##GridFilter` id stays (UI tests key on it); the box stays on `TextBuf` (the dictation router needs the fixed buffer).
- **Inline inventory**: `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp:296-337` (the box); `Source/Core/src/Ui/SmatchetGridPaneWindows.cpp:253` (the replay); `Source/Core/include/Ui/SmatchetUiSession.h:249` (`JqlEditorState`), `:292,307` (its futures), `:994` (`PaneDeferredActionKind`); `Source/Core/include/RecentSelections.h:22-26` (the caps).
- **Order**: after S3 and S4, and after #2439 and #2452 merge (`SmatchetUI.cpp`).

**S6 — facet chips**
- **Owner subsystem**: `Source/Core/src/Ui/` (grid), with one registry entry in `Source/Core/src/Commands/`. Kept in one slice: the command only sets a pane latch this slice introduces, and it cannot be tested without the menu.
- **Write set**: items 28-32.
- **Pre-resolved decisions**: chips edit the view's query through `applyQueryToPaneView`, never `ViewState` directly (Ui leaf § Grid: the dashboard owns view CRUD; this is an in-grid gesture on the live view, saved by the strip); the apply is latched and replayed after the pane loop; a multi-select picker applies on close; pickers read the pane's catalog, the saved roster and, for an empty label list, the labels of the loaded rows, built when the popup opens; a Plane pane draws + Filter disabled with its reason; the command handler runs on the UI thread like `grid.search.focus` (`ui-request-flag-off-thread`).
- **Inline inventory**: `Source/Core/src/Ui/SmatchetGridSearchUi.cpp:160-192` (`applyQueryToPaneView`); `Source/Core/src/Commands/Builtin/BuiltinCommands_Ui.cpp:147` (`grid.search.focus`, the shape to copy); `Source/Core/include/FilterDropdown.h:71-81`.
- **Order**: after S1 and S5.

**S7 — the close-matches hint**
- **Owner subsystem**: `Source/Core/src/Ui/` (grid).
- **Write set**: items 33-36.
- **Pre-resolved decisions**: one `bool` set where the fallback already runs, no per-row work; the hint has the lowest priority in `PickGridSearchLine`.
- **Inline inventory**: `Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp:252` (`ApplyGridFilterProjection`); the debt entry's "Concrete next action".
- **Order**: after S3; after #2452 and after the sibling plan's slice that edits `SmatchetActiveProjectGridTable.cpp`, or before it by agreement, never in parallel.

## Verification

Per `AGENTS.md` § Verification automation: zero manual steps.

- **Bucket A (pure-logic doctest, `test-authoring` skill § Unit rig)**:
  - S1 `QueryFacetClausesPure.test.cpp`: per dialect, parse → set → remove returns the original bytes; OR, NOT and grouping are not chippable; `ORDER BY` is kept; quoting of values with spaces and quotes; `DialectSupports` rejects a GitHub multi-label and a GitHub `assignee !=`; a wrapped non-chippable query.
  - S2 `TicketSyncService.test.cpp`: `Warning`, `ErrorTransient`, `Cancelled`, `ViewId` and `Query` reach the stats.
  - S3 `GridSearchBoxPure.test.cpp` and `GridSearchInputClassifier.test.cpp`: line priority; the watch (stale request id, another view's or another query's session, transient error, cancelled session, warning, clean session); the Linear key and query cases.
  - S4 `AutocompleteListNav.test.cpp`: `CommitOnPick`, the memo skip, hover.
  - S5 `GridSearchBoxPure.test.cpp` and `RecentSelections.test.cpp`: the suggestion gate (a title word, a field prefix, a query, a non-Jira pane); query-scope limits and the `FromJson` re-cap; other scopes still 5 of 256.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**:
  - S3 `TolerantParse`: an unbalanced parenthesis shows the line and Enter leaves the view's query unchanged; `GridSearch`: the deterministic backend rejects a query and its text appears under the box.
  - S5 `GridSearch`: typing `sta` offers `status` and Enter still filters titles; an empty focused box lists recents and Down + Enter applies one; no user search is sent offline; recents survive a store reload (`BeginSharedTestScope` / `EndSharedTestScope`).
  - S6 `GridFacetChips`: adding a status chip rewrites the view's query and raises the strip; removing it restores the query; a Plane pane's + Filter is disabled; the GitHub label picker is single-select; `grid.search.add_filter` opens the menu.
  - S7 `GridSearch`: the close-matches hint appears when only the fallback matched.
- **Bash-driver scenario / screenshot / sanitizer**: the existing sanitizer lanes cover the new pure TUs; no new golden image (the chip row has no bucket-C scenario; bucket E asserts it by item).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) for every slice.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green for this PR and for every slice that edits a doc. A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run against the Tracker glossary and the two leaf rule files before this plan is final; the outcome is recorded in this PR's description and in § Open questions.
- **Manual residue**: none planned. A slice that ends with a manual step names its automation follow-up and files a `docs/self-improvement/categories/tooling/` entry.

## Open questions

- **Q1 (user):** a Plane pane gets no chips, because a Plane view query cannot filter by field. Is a disabled + Filter with that reason enough for now, or should a follow-up plan add a local filter over the loaded rows for Plane?
- **Q2 (user):** GitHub and Linear panes get chips but no typed suggestions. Suggestions there need the Jira engine limited to the fields each translator supports. Wanted as a follow-up?
- **Q3 (user):** should changing a chip also add the resulting query to the recent list? The plan says no, so the list holds only what was typed.
- **Q4 (probe, S5):** is a dedicated `grid-search-typing` perf scenario worth adding, or do the two permanent markers under `side-by-side-2-grid` suffice?

**Resolved by the `grill-with-docs` pass (2026-11-20), against the code:**

- *Glossary.* No `CONTEXT.md` uses "facet" or "chip", so the two new terms are free; the Tracker glossary's "Plane filter mini-language" entry agrees with the Plane column above. The glossary lines are written in S1, with the code they name.
- *Whose stats?* `GetLastSyncSessionStats` returns the focused tracker context's last finished session, so the watch is keyed on the pane's cache key and polled only while that context is focused.
- *Could an older session be read as the answer?* Yes, with a request-id check alone. The stats now carry the view id and the query, and the watch matches on both.
- *Is every non-transient error a rejection?* No. A cancelled session records a fixed error text, so it gets its own `Cancelled` flag and shows nothing. A sign-in failure or a worker exception is also non-transient; the line's wording is "The tracker did not run this query: …" followed by the tracker's own text, which stays true for those.
- *Linear validation.* The validator's built-in field names do not include `team`, so a Linear pane is checked for structure only. A clause Linear ignores is reported after the sync.
- *GitHub `type:pr`.* A `word:value` token is kept as an opaque clause, so a pull-request view can still take chips.
- *Does any fetch end outside the streaming session?* No: `ApplyIssueFetchPack` has no caller under `Source/Core/src`.
- *Storage.* Recents are a JSON array in the config file, not a table; no migration.
- *ADR?* None. No storage shape or interface changes, and every choice here can be reversed in one slice.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **#64, one meaning for Enter in the Views editor**: a behaviour change to the dashboard; follow-up plan.
- **#65, switching views by keyboard**, and **#70, Clear also discarding the applied query**: separate small items; no action here.
- **A Plane filter**, server-side or local (Q1), and **typed suggestions on GitHub and Linear** (Q2): follow-up plans once the questions are answered.
- **A warning before Enter that GitHub or Linear will ignore a clause**: possible by running the pure translators on the typed text; this plan reports it after the sync instead. Follow-up.
- **Saved views listed inside the search box**: the view picker beside the box already lists them.
- **Chips for custom, date and number fields; OR and grouping in chips; typo tolerance for clause values**: non-goals of this plan.
- **`userInfoAddToQuery` compares the pane's backend key with lowercase `"plane"`** (`Source/Core/src/Ui/SmatchetUI.cpp:1361`) while the normalized key is `"Plane"`, so a Plane pane gets the JQL key form. Found while planning; a product bug for a GitHub Issue, not designed here.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agent-layer/agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md`, so the move can't break them.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
