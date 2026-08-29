# jql-username-in-input — show display names in the JQL input, keep wire/disk id-canonical

## Status
Active. Branch `claude/jql-name-in-input` off `origin/develop` (2fd8e2af2).

## Problem
Picking a user from the async live-search dropdown inserts the raw Jira account id
(`assignee = 5d52c6f4f386720da657c7d8`) into the JQL input. A grey "reads as: …" echo
line under the bar translated ids to names, but the user rejected that design:

> "remove the echo. I want the show text to show the user username"

## Design (Option 1 — buffer holds display names; wire/disk stay id-canonical)
1. **Async rows insert names** — `ReduceUserSearchResultIntoItems` switches from
   `InsertForValueToken(u.AccountId)` to the exported `BuildJqlUserInsert(u)` (the
   same helper the catalog path uses), so async and catalog rows are byte-identical
   and dedup-by-Insert keeps working. Labels become catalog-style
   `DisplayName (EmailAddress)`.
2. **Beautify pass** — `TrackerQueryAcp_DrawUserEcho` becomes
   `TrackerQueryAcp_ApplyUserNamesToBuffer`: when the input is NOT hot, no pending
   replace is queued, and the backend is Jira, rewrite `st.buf` id→name in place via
   `RenderQueryWithUserNames` (catalog pass then search-resolved pass — safe forward,
   ids are unique keys). Sets `jqlBufSemanticRewrite` so the views dirty-compare can
   ignore the rewrite. Memoised exactly like the old echo (renamed fields
   `jqlNameRewrite*`) — steady frame is one string compare + two scalar compares.
3. **Reverse map at apply boundaries** — new pure
   `jql_user_display::RenderQueryWithAccountIds(query, fields, users, outReplaced)`:
   field-aware state machine over `ForEachValueToken` that replaces unique
   case-insensitive display-name matches in user-field value position with
   `InsertForValueToken(AccountId)`. Called (Jira-gated) at:
   - `BuildUpdatedView` (covers viewsApplyAndSync + viewsCreateNewView — `app`
     threaded through viewsCreateNewView),
   - Open-in-browser URL build,
   - `applyOmnibarEnter` Jql case.
   Name→id resolution runs over ONE merged users vector (catalog +
   `jqlAcpSearchResolvedUsers`) — two-pass compose would wrongly treat a duplicate
   name split across the lists as unique.
4. **Echo removal** — delete the echo render + tooltip + decl + `jqlUserEcho` field.

### State-machine rules (RenderQueryWithAccountIds)
- Clause-break keywords `and`/`or`/`order` reset user-field state.
- Field token (`FindTrackerField` + `IsQueryUserField`) enters user-value state;
  a non-user field clears it.
- In-state keeper keywords `in`/`not`/`is`/`was`/`changed`/`empty`/`null` pass through.
- In-state precedence: keeper keyword → `LooksLikeAccountId` (verbatim) → unique
  case-insensitive DisplayName match (replace, keep state for `in (a, b)` lists) →
  field match (state switch) → verbatim.
- Ambiguous duplicate names left verbatim (Jira errors clearly); unknown names pass
  through (Jira Cloud accepts name-form for user fields anyway).

## Files to modify
| File | Change |
|---|---|
| `Source/Core/src/Tracker/JqlUserDisplayPure.cpp` | add `RenderQueryWithAccountIds` state machine + name-lookup helper |
| `Source/Core/include/Tracker/JqlUserDisplayPure.h` | decl + `TrackerField` fwd-decl; rewrite "readable ECHO" namespace doc |
| `Source/Core/src/Tracker/JqlSuggestEnginePure.cpp` | export `BuildJqlUserInsert` (drop `static`) |
| `Source/Core/include/Tracker/JqlSuggestEnginePure.h` | `BuildJqlUserInsert` decl |
| `Source/Core/src/Ui/SmatchetAutocompleteUi.cpp` | async rows use `BuildJqlUserInsert`; echo → `TrackerQueryAcp_ApplyUserNamesToBuffer`; add `TrackerQueryAcp_QueryWithAccountIds` merge helper; memo-field renames |
| `Source/Core/include/Ui/SmatchetAutocompleteUi.h` | decl swap |
| `Source/Core/include/Ui/SmatchetUiSession.h` | drop `jqlUserEcho`; rename memo → `jqlNameRewrite*`; add `jqlBufSemanticRewrite` |
| `Source/Core/src/Ui/SmatchetViewsDashboardUi_widgets.cpp` | echo call → Jira-gated beautify call |
| `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp` | `BuildUpdatedView` reverse-map (+`app` param); Open-in-browser wrap; dirty-compare consumes `jqlBufSemanticRewrite` |
| `Source/Core/include/Ui/SmatchetUI.h` | `viewsCreateNewView` gains `app` |
| `Source/Core/src/Ui/SmatchetOmnibarUi.cpp` | Jira-gated reverse-map in `applyOmnibarEnter` |
| `tests/Core/JqlUserDisplayPure.test.cpp` | `RenderQueryWithAccountIds` cases |

## Perf-gate (mandatory — diff touches Source/Core/)
- **Steady frame (Pillar 1)**: the beautify pass is memoised on
  (`buf`, catalog data ptr, catalog size) exactly like the old echo — a steady frame
  costs one `std::string` compare + two scalar compares, no allocation. The rewrite
  itself runs only when the buffer or catalog actually changed AND the input is not
  focused (i.e. after a pick/apply, not per keystroke).
- **Reverse map** runs only on user actions (Apply / Create / Enter / Open-in-browser),
  never per frame. The merged users vector copy is bounded by catalog size and happens
  on those actions only.
- No new per-frame allocations on the hot path; no new locks; UI thread never blocks.

## Verification
- Doctest: `RenderQueryWithAccountIds` (replace incl. `:`-forced quoting; text-field
  values untouched; ambiguous verbatim; already-id verbatim; `in (…)` lists;
  case-insensitive field; unknown name verbatim; `and`-reset; no-space
  `=5d52…` round trip).
- Delta lint: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop`.
- Manual (visual-validation exception — touches `Smatchet*Ui*.cpp`, no bucket-C/E
  JQL/autocomplete coverage): build manual-test tree, launch, user verifies the input
  shows `assignee = "Alex Konstantonis"` after an async pick and the applied query
  still hits Jira correctly.

## Implementation log
_(filled post-ship)_

## Deviations
_(none yet)_
