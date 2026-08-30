# jql-username-in-input — show display names in the JQL input, keep wire/disk id-canonical
<!-- plan-date: 2026-08-30 -->

## Status
Shipped. PR #2176 squash-merged to `develop` 2026-08-30 (`17aa5c63f`).
Branch `claude/jql-name-in-input` deleted (remote auto-delete on squash; local removed post-merge).

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
  2026-08-29: user verdict on build `eb90f3d7`: "it works" (after the duplicate-row
  fix; earlier verdict on `843536b9` reported the two-rows-per-user defect).
- Final (head `b335714be`): full unit suite 3031 cases / 42992 assertions, 0 failed;
  `Smatchet.exe` links (`ninja-iter-msvc`); delta lint gates all PASS (advisory WARNs only).
  Merge gates 22/22 CI green, CodeRabbit 0 open findings, Bugbot 0 open, 0 user threads.

## Implementation log
- 2026-08-29: echo (`TrackerQueryAcp_DrawUserEcho`) deleted; input buffer now holds display
  names via `TrackerQueryAcp_ApplyUserNamesToBuffer` (idle-only: not focused, no pending
  replace, Jira-only), memoised through `jqlNameRewrite*` + consumed-by-dirty-compare
  `jqlBufSemanticRewrite`.
- Reverse map name->id at every apply boundary: dashboard `BuildUpdatedView` (both save
  paths, `app` threaded through decl/def/2 lambdas/2 direct calls), open-in-browser URL,
  omnibar Enter (gated on the PANE's backend, not `d.cfg.TrackerType`), grid unsaved-strip
  Save, grid Save-as-new.
- `RenderQueryWithAccountIds` doctests: 6 TEST_CASEs (name->id, non-user/id/unknown
  passthrough, IN-list + clause breaks + field-switch, unique-vs-ambiguous names,
  degenerates) — 15 jql_user_display cases / 75 assertions green.
- 2026-08-29 (post-manual-test fix): user reported "two entries in the menu for each user,
  one works, one doesnt" — root cause: `TrackerFieldCatalog` mirrors the whole user catalog
  into every user field's `AllowedValueOptions`/`AllowedValues`, so `AppendValueSuggestions`
  emitted a second, raw-accountId-inserting row beside each catalog row (the id insert
  deduped against nothing; picking it left the opaque hash in the focused input, which the
  idle-only rewrite never touches). Fix: `AppendJqlValueModeSuggestions` gates the
  allowed-value rows with `!IsQueryUserField(*valueField)` — catalog + async search are the
  only user-row sources, both insert the same `BuildJqlUserInsert` token, dedup to one row.
  Plane call site / shared common body untouched. Pinned options-rows TEST_CASE rewritten to
  assert exactly one name-form row, no raw-id row. Full suite 3028 cases / 42978 assertions
  green. Intended side effects: empty-prefix `assignee = ` no longer dumps the whole org
  list (matches the catalog path's empty-prefix bail-out design); options-only users with no
  catalog backing are no longer suggested (their name insert could never reverse-map).
- 2026-08-29 (review-refinement pass, addresses the 2 Bugbot findings on PR #2176): both
  mapping directions now share ONE clause-aware walker (`RewriteUserFieldValues` template in
  JqlUserDisplayPure.cpp) so id->name and name->id are exact inverses — a token is only
  rewritten in a position the opposite direction would rewrite back. Fixes: (1) function
  arguments no longer map in EITHER direction (`funcDepth` short-circuit + paren-kind stack
  'f'/'l'/'b' — a quoted `membersOf("Group")` arg can no longer be rewritten to an account
  id); (2) quoted field names now arm the user-value state (`"Assignee" = Jane Doe` maps
  both ways; `FindTrackerField` matches Id then Name on the unquoted token); cf[...] clauses
  stay untouched in BOTH directions (symmetric — `cf` never resolves as a field token).
  id->name direction is now field-catalog-gated: only user-type field value positions
  rewrite, empty catalog = fail-safe no-op; rewrite memo keyed on the fields snapshot too
  (`jqlNameRewriteFieldsData/Size` in SmatchetUiSession.h). Omnibar clears
  `jqlBufSemanticRewrite` after draw (NIT-b). Tests migrated to the 4-arg field-gated
  signature + new position-gating and byte-for-byte round-trip cases — 17 jql_user_display
  cases / 85 assertions green; full suite 7/7 ctest lanes green; lint gates all PASS
  (advisory WARNs only: pre-existing tu-line-ceiling, func-size soft 103>100 on the walker,
  comment-ratio on two headers).

- 2026-08-30 (CR-triage pass, addresses the 1 confirmed-Major CodeRabbit finding on PR
  #2176): the id->name direction now carries a round-trip guard — `RenderQueryWithUserNames`
  rewrites an id to a display name only when `UniqueAccountIdForName` maps that name back to
  exactly this id across the supplied `users`, so a display name shared by two accounts keeps
  BOTH ids as typed (naming either would render a query the name->id inverse refuses to undo).
  Call site (`TrackerQueryAcp_ApplyUserNamesToBuffer`) reworked from two sequential passes
  (catalog, then search-resolved over the first pass output — each pass blind to the other
  list, so cross-list duplicate names looked unique) to ONE merged catalog+search-resolved
  vector, mirroring `TrackerQueryAcp_QueryWithAccountIds`; merge cost is memo-miss-only
  (Pillar 1). Header doc pins the ONE-merged-vector contract. New doctest: ambiguous name
  keeps id / same account listed twice still counts as one. 3031 cases / 42992 assertions
  green. Commit `b335714be`.

## Deviations
- **Scope add — `TrackerQueryAcp_CanonicalQueryForApply`** (SmatchetAutocompleteUi.{h,cpp}):
  wrapper folding the Jira gate + reverse map so the 4 cfg-gated boundaries are one-call
  sites (DRY, keeps `TrackerBackendKind.h` out of 3 TUs). Omnibar deliberately bypasses it:
  its gate vocabulary is the pane's `OmnibarBackend`, not `cfg.TrackerType`.
- **Scope add — grid Save boundaries**: plan listed only the dashboard boundaries; the grid
  unsaved-strip Save and Save-as-new popup also persist `viewJqlEditor.buf` and needed the
  same reverse map.
- `omniJqlEditor.jqlBufSemanticRewrite` has no consumer (omnibar has no dirty-compare);
  the omnibar now clears it after its hint draw so the latch cannot leak a stale `true`
  into any future consumer.
