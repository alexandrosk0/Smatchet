# Manual test queue

> Smoke tests that aren't covered by any PR's automated build verification. Each entry is something a human has to actually click / run / observe at a keyboard. Tests are grouped by the PR that introduced the behaviour they validate.
>
> When you complete a smoke, flip ⏳ → ✅ in place and add a one-line note (date + result + tester initials). If a smoke regresses, flip to ❌ and file a new backlog item.
>
> ## ⚠️ Staleness note (2026-07-05)
>
> **43 entries; ~40 still ⏳ PENDING** (1 ✅, 1 🟡, 1 ❌) — the queue has sat essentially unexercised. Every entry references early single/double-digit PRs (#12–#22) against develop tips `93f561b` / `0a79de5`, now buried ~1600 merged PRs deep (current tip ~#1620). In particular the **build-re-verify commands (B1/B2) pin a baseline (`93f561b`) that no longer means anything** — treat those as historical, not runnable as written. The per-PR runtime smokes below validate behaviours that mostly still exist but have been substantially reworked since. Before running any of these, re-baseline against current `develop`, or treat the pre-#22 batch as *superseded / historical*. This is a human-only queue; nothing here was auto-verifiable in the reconciliation pass.

---

## Status legend

- ⏳ **PENDING** — not yet exercised against the merged change.
- ✅ **PASSED** — exercised; observed the expected behaviour.
- ❌ **FAILED** — exercised; observed something different. File a backlog item with the divergence.
- 🟡 **PARTIAL** — exercised part of the smoke; remaining steps still pending.

---

## Build re-verify on current develop tip

The build hook only fires on individual file edits; PR merges don't trigger a full target build. After the cascade of P1-cleanup + cppcheck-cleanup PRs lands, re-verify both targets.

- ⏳ **B1 — Standalone + DX12 core both compile clean.**
  - **Command**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`
  - **Expected**: 0 errors, 0 new warnings vs the develop tip pre-cleanup (last clean baseline: `93f561b`).
  - **Why it matters**: PRs #12 / #15 / #16 each rebased against a moving develop; the post-merge tip was never validated as a single unit.

- ⏳ **B2 — DX12 ImGui host links clean** (only if #12's item 22 is being smoked).
  - **Command**: `cmake --build --preset ninja-iter-msvc --target SmatchetImGuiHost_DX12`
  - **Expected**: clean build of the DX12 host static lib.

---

## From [PR #12](https://github.com/alexandrosk0/Smatchet/pull/12) — P1 cleanup batch

Four runtime fixes, each needs an independent smoke.

- ⏳ **M1 — MCP SSE heartbeat shutdown latency (item 9).**
  - **Setup**: Build standalone; enable MCP server (`Mcp` plugin started). Connect one (or several) SSE clients to `/mcp/sse`. The MCP Inspector tool or a `curl -N http://localhost:<port>/mcp/sse` works.
  - **Action**: While clients are connected, quit Smatchet (window close or `Alt+F4`).
  - **Expected**: Process exits within ~100 ms of the quit. Watch the OS process list — `Smatchet.exe` should be gone almost immediately, not lingering for 1+ second per connected client.
  - **Pre-fix behaviour**: shutdown blocked up to 1 s per connected client (sleep-based heartbeat).
  - **Reaches**: also exercises the `Impl` member-order hardening from `fix(mcp): order Impl members so shutdown primitives outlive ~Server` (PR #12, commit `6053c09`) — workers complete cleanly during `~Server`'s join even if they're mid-lambda.

- ⏳ **M2 — `CellIdScope` cell-ID collision on empty `FieldId` (item 10).**
  - **Setup**: Force a row whose catalog has no resolvable field id for one or more columns — easiest reproduction: connect to a tracker with synthetic / errored field metadata, or create a custom view that references a deleted field id.
  - **Action**: Click on an editable cell whose `FieldId` is empty. Open a popup (dropdown, datepicker, multi-select). Click an adjacent cell in the same row that also has empty `FieldId`.
  - **Expected**: Edit state stays per-cell. The popup that opened for cell A does NOT also open for cell B; field-edit text from cell A does not leak into cell B.
  - **Pre-fix behaviour**: `PushID("")` hashed identically for every empty-field column, so all empty-`FieldId` cells in a row collapsed onto one ImGui widget id — edit state and popups leaked between cells.

- ⏳ **M3 — Grid invalidation on in-place view edits (item 18).**
  - **Setup**: Open the Active Project window with a view that has multiple columns. Make sure the catalog revision is stable (no pending tracker refresh).
  - **Action**:
    1. Drag a column-width handle and release.
    2. Click another column header to change the sort.
    3. Re-order columns via the view editor (if exposed).
  - **Expected**: On the very next frame, the grid reflects the new layout / sort. No need to switch views or trigger a catalog refresh to see the change.
  - **Pre-fix behaviour**: `GridFrameContext`'s cache key was `(catalogRevision, activeViewId)` only — in-place edits that left both unchanged kept stale columns until the catalog revision bumped.

- ⏳ **M4 — DX12 backend recovery after `UpdateRendererColorFormat` failure (item 22).**
  - **Setup**: Standalone uses the OpenGL backend, so this smoke only applies if you can exercise the DX12 host. Hard to reproduce without an Unreal embedding harness.
  - **Action** (DX12 path only): Force `Smatchet_ImplDX12_InitBackend` to fail on a color-format change — e.g., temporarily edit it to return false unconditionally, run, then revert. Drive a color-format change in the UI.
  - **Expected**: `LastInitError` is populated; subsequent frames retry `Initialize()` (~1 Hz while the UI is visible) instead of dispatching draw calls into a torn-down backend. No crash, no rendering glitches that persist past the first successful retry.
  - **Pre-fix behaviour**: `Initialized` stayed true while the backend was actually torn down — every `DrawUI` / `RenderDrawData` after the init-fail walked into a dead backend.
  - **Skip if**: you have no DX12 harness. Mark this row "N/A — no DX12 harness available" rather than ⏳ in that case.

---

## From [PR #15](https://github.com/alexandrosk0/Smatchet/pull/15) — Plane `CreateIssue` cfg snapshot

- ⏳ **M5 — Plane issue creation under credential rotation (item 12).**
  - **Setup**: Configure Plane as the active tracker with valid credentials. Have a means to rotate the Plane API token mid-flight (a second editor process, or a Lua automation that calls `ConfigManager::Save()` with a different token).
  - **Action**:
    1. Initiate `CreateIssue` from the UI (new-issue dialog → submit).
    2. While the POST is in flight, rotate `PlaneApiToken` (and/or `PlaneWorkspaceSlug`) via the second path.
  - **Expected**: The in-flight POST either succeeds with the snapshot it captured, or fails with a coherent error. The cache lookup (`planeProjectId_` / `planeProjectIdentifier_`) and the request headers came from the SAME `TrackerConfig` snapshot taken under `planeCacheMutex_`.
  - **Pre-fix behaviour**: `cfg` was captured before the lock; the cache lookup happened under the lock; mid-flight rotation could send stale auth while reading fresh cache.
  - **Lighter smoke (no rotation)**: just create an issue normally and confirm it lands in Plane with the right project + reporter. Validates the function still works end-to-end after the lock-scope change.

---

## From [PR #16](https://github.com/alexandrosk0/Smatchet/pull/16) — audit-trail fallback

- ⏳ **M6 — Audit fallback when primary file is unwritable (item 20).**
  - **Setup**: Quit Smatchet. In the user-data directory, take the primary audit file unwritable (Windows: right-click → Properties → Read-only, or `attrib +r smatchet_backend_audit.jsonl`; POSIX: `chmod 0444`).
  - **Action**: Start Smatchet, perform any backend mutation that produces an audit event (create / edit / delete a ticket, or run a Lua automation that triggers AppendEvent).
  - **Expected**:
    1. A `smatchet_backend_audit_fallback.jsonl` appears in the same directory.
    2. The mutation's JSONL line is appended to the fallback file with the same shape as the primary.
    3. The Logger ring has exactly ONE error line about the primary open failure (rate-limited).
  - **Cleanup**: Restore write access to the primary file.

- ⏳ **M7 — Both audit paths unwritable (item 20, secondary leg).**
  - **Setup**: Make BOTH `smatchet_backend_audit.jsonl` and `smatchet_backend_audit_fallback.jsonl` unwritable, or make the entire user-data directory read-only.
  - **Action**: Trigger 1, then 10, then 100 audit-producing mutations.
  - **Expected**: Logger ring shows `LOG_ERROR` lines at counts 1 / 10 / 100 (log-spaced multiples) reporting dropped events. No spam between those checkpoints.
  - **Cleanup**: Restore write access.

---

## From [PR #17](https://github.com/alexandrosk0/Smatchet/pull/17) — P2 polish batch (open)

- ⏳ **M8 — `Views::DeleteActive` neighbour pick (item 31).**
  - **Setup**: Create three or more views: `[A, B, C, D]`. Activate `C`.
  - **Action**: Delete the active view (`C`).
  - **Expected**: New active view is `D` (the neighbour at the same index after erase). Repeat with the last view `D` active: deleting `D` should land on `C`, not `A`.
  - **Pre-fix behaviour**: Deleting any view always jumped active to `Views.front()` (i.e. `A`).

---

## From [PR #21](https://github.com/alexandrosk0/Smatchet/pull/21) — cppcheck cleanup (open)

These are regression-only smokes — the PR is pure cleanup, so the contract is "everything that worked before still works." Light touch.

- ⏳ **M9 — MCP REST / JSON-RPC `tools/call` paths still respond.**
  - **Setup**: Standalone with MCP enabled.
  - **Action**:
    1. `curl -X POST http://localhost:<port>/mcp/tools/call -d '{"name":"search_active_tickets","arguments":{"query":"foo"}}'`
    2. `curl -X POST http://localhost:<port>/mcp -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"search_active_tickets","arguments":{"query":"foo"}},"id":1}'`
  - **Expected**: Both return a non-error response with `content` populated. The `result` hoist and the `argsStr`/`error` scope tighten don't change observable behaviour.
  - **Bonus**: With `SMATCHET_WITH_LUA_AUTOMATION` defined, also exercise `run_lua` via REST + JSON-RPC.

- ⏳ **M10 — Grid filter still filters.**
  - **Setup**: Active Project window with a view containing many rows.
  - **Action**: Type into the grid filter box; clear it; type something else.
  - **Expected**: Filter applies on each change; `lastFilter` is updated each frame; no stale state. Validates the strncpy + null-term change at `SmatchetActiveProjectGridUi.cpp:439`.

- ⏳ **M11 — DPAPI base64 round-trip still works.**
  - **Setup**: Standalone on Windows with MCP enabled, `McpAuthToken` configured.
  - **Action**: Set a non-empty `McpAuthToken` via the settings UI; save; restart; verify the token persists (re-encryptable round-trip via `BinaryToBase64` / `Base64ToBinary`).
  - **Expected**: Token survives save/load. Validates the `outLen == 0` guard and the dropped `!out.empty()` predicate in `ConfigManager.cpp::BinaryToBase64`.

- ⏳ **M12 — Plane field type-mapping unchanged.**
  - **Setup**: Plane as active tracker; a project with at least one of each field type (URL, RICH_TEXT, TEXT, HTML, plus unknown types if you have them).
  - **Action**: Browse the field catalog / display fields in the grid.
  - **Expected**: All such fields render as text-like (Family = `Text`). The branch collapse in `PlaneClient.cpp:339-348` didn't change behaviour — every type that fell through the three duplicate branches before still falls through the single `else` now.

---

## Views window redesign (two-pane settings editor)

- ⏳ **V1 — Sidebar list of saved views + active highlight.**
  - **Setup**: Standalone with at least 3 saved views.
  - **Action**: Open the Views window; click each view in the left sidebar; type into the search box.
  - **Expected**: Active view marked with leading "* "; clicking another view loads its name / JQL / fields / column order into the right pane; search filters the list case-insensitively.
  - **Why it matters**: Replaces the legacy single-line `BeginCombo`. No tests cover sidebar selection / search.

- ⏳ **V2 — Tabbed editor: Filter / Fields / Columns / Sort.**
  - **Setup**: Views window with an active view that has fields, columns, and at least one sort spec.
  - **Action**: Click through each tab; switch to a different view; come back.
  - **Expected**: Each tab shows the right body; switching views resets dirty flag; the selected tab persists across view switches within a single session.

- ⏳ **V3 — Drag-and-drop reorder in Fields / Columns / Sort tabs.**
  - **Setup**: Any view with ≥ 3 selected fields and ≥ 3 columns.
  - **Action**: Grab the ⋮⋮ handle on a row and drop on another row in each of the three tabs.
  - **Expected**: Row moves to the drop position; dirty indicator lights up; Apply & Sync persists the new order; legacy Move Up/Move Down buttons are gone.

- ⏳ **V4 — Keyboard reorder via Alt+↑ / Alt+↓.**
  - **Setup**: Same as V3.
  - **Action**: Click a row in Selected (Fields tab), Column Order (Columns tab), or Sort tab to focus it; press Alt+↑ or Alt+↓.
  - **Expected**: Focused row swaps with its neighbour; focus follows the row across the swap; no swap past list ends.

- ⏳ **V5 — Dirty tracking + Discard-changes modal on view switch.**
  - **Setup**: Open a view, edit name / JQL / a field checkbox.
  - **Action**: Click a different view in the sidebar.
  - **Expected**: "Discard changes?" modal appears with Save & switch / Discard & switch / Cancel. Cancel keeps you on the current view with edits intact; Save & switch persists then switches; Discard & switch drops edits.

- ⏳ **V6 — Delete view confirm modal + Delete button styling.**
  - **Setup**: Views window with ≥ 2 saved views.
  - **Action**: Click "Delete view".
  - **Expected**: Confirm modal asks "Delete view \"X\"? This cannot be undone." with a red-styled Delete button. Cancel keeps the view; Delete removes it and activates a neighbour.

- ⏳ **V7 — Sort tab: direction cycle and add-key popup.**
  - **Setup**: View with at least 2 columns selected.
  - **Action**: In Sort tab, click "+ Add sort key", pick a column. Click the direction button on the new row to cycle — → Asc → Desc → —. Apply & Sync.
  - **Expected**: Grid re-sorts in the chosen direction; SortSpecs persist across restart.

- ⏳ **V8 — Sidebar splitter + Fields-pane splitter persist across restart.**
  - **Setup**: Views window with default widths.
  - **Action**: Drag the sidebar splitter wider; switch to Fields tab; drag the Available/Selected splitter; restart the app.
  - **Expected**: Both widths restore on next launch (persisted as `views_sidebar_width` / `views_fields_split_ratio` in smatchet_config.json).

- ⏳ **V9 — Ctrl+Enter / Ctrl+N shortcuts.**
  - **Setup**: Views window focused.
  - **Action**: With unsaved edits, press Ctrl+Enter; press Ctrl+N.
  - **Expected**: Ctrl+Enter runs Apply & Sync (toast "View saved"); Ctrl+N creates a new view (toast "View created").

- ⏳ **V10 — Toasts fire on save / create / duplicate / delete / discard.**
  - **Setup**: Views window.
  - **Action**: Apply & Sync; right-click a row → Duplicate; right-click → Delete; edit then Discard.
  - **Expected**: Each action triggers a single toast with the view name and appropriate type (Success / Info).

- ⏳ **V11 — Grid column reorder by header drag.**
  - **Setup**: Active Project window with at least 4 columns visible.
  - **Action**: Click and drag a column header sideways to reorder it; release.
  - **Expected**: Column lands in the dropped position; an "● Unsaved layout changes to \"<view>\"" strip appears at the top of the grid with [Save] / [Save as new...] / [Discard] buttons; legacy autosave for widths/sort still fires silently.

- ⏳ **V12 — Unsaved-layout strip: Save commits the order to the active view.**
  - **Setup**: V11 dirty state.
  - **Action**: Click [Save].
  - **Expected**: Strip disappears; toast "View saved"; restart the app and confirm the new column order is restored from disk.

- ⏳ **V13 — Unsaved-layout strip: Save as new... forks the view.**
  - **Setup**: V11 dirty state.
  - **Action**: Click [Save as new...]; modal opens with "<name> (copy)" pre-filled; press Enter or click Save.
  - **Expected**: New view created with the reordered columns; sidebar in the Views window shows the new view as active; toast "View created"; original view's ColumnOrder is unchanged on disk.

- ⏳ **V14 — Unsaved-layout strip: Discard reverts to stored order.**
  - **Setup**: V11 dirty state.
  - **Action**: Click [Discard].
  - **Expected**: Columns snap back to the active view's stored ColumnOrder; strip disappears; toast "Reverted layout".

- ⏳ **V15 — Dirty state crosses windows.**
  - **Setup**: Both Active Project and Views windows visible.
  - **Action**: Drag a column header in the grid to reorder.
  - **Expected**: Unsaved strip appears in the grid AND the Views window title shows "  unsaved" + Discard button enables. Clicking either window's Save commits in both places.

- ⏳ **V16 — Column resize now gates behind Save.**
  - **Setup**: Active Project grid, no unsaved state.
  - **Action**: Drag a column edge to resize one or more columns.
  - **Expected**: The columns visibly resize *and* the unsaved-layout strip appears. Restart the app **without** clicking Save → original widths come back. Repeat, this time click Save → toast "View saved", restart, the new widths persist.
  - **Pre-fix behaviour**: Column widths silently autosaved via a 300 ms debounce regardless of whether the user wanted to keep them.

- ⏳ **V17 — Header sort click gates behind Save.**
  - **Setup**: Active Project grid, no unsaved state.
  - **Action**: Click a column header to sort by it (asc → desc → none).
  - **Expected**: Grid re-sorts immediately and the unsaved-layout strip appears. Discard → sort reverts to the stored one. Save → toast "View saved" and the new sort persists across restart.
  - **Pre-fix behaviour**: Header sort autosaved silently via the same 300 ms debounce.

- ⏳ **V18 — Sort By popup mutations gate behind Save.**
  - **Setup**: Active Project grid, no unsaved state.
  - **Action**: Open "Sort By ↕" popup; click X to remove a sort rule, or toggle Ascending/Descending, or "+ Add Sort Rule".
  - **Expected**: Each mutation triggers the unsaved-layout strip. Discard → all popup-driven mutations revert. Save → they persist.

- ⏳ **V19 — Discard reverts widths + sort + column order together.**
  - **Setup**: Active Project grid with a dirty layout containing (a) at least one resized column, (b) at least one sort spec change, and (c) one column reorder.
  - **Action**: Click Discard.
  - **Expected**: A single Discard reverts all three concerns to the on-disk values (the pre-dirty snapshot captures the full ViewDefinition).

- ⏳ **V20 — Debounced auto-save does not fire while dirty.**
  - **Setup**: Active Project grid with a dirty layout (any of: resize, sort, reorder).
  - **Action**: Wait several seconds without clicking Save or Discard, then kill the process (Task Manager) without a clean exit.
  - **Expected**: On next launch the original layout is restored (debounced save was skipped because `viewsDirty == true`). Pre-fix this would have silently saved within ~300 ms.

- ⏳ **V21 — JQL function suggestions surface by field family.**
  - **Setup**: Jira active, Views editor → Filter tab, JQL input focused.
  - **Action**: Try each token below; observe the autocomplete popup:
    1. `assignee = cu` → `currentUser()` + `membersOf("…")` appear (user functions).
    2. `created > st` → `startOfDay()` / `startOfWeek()` / `startOfMonth()` / `startOfYear()` appear (date functions).
    3. `created < no` → `now()` appears.
    4. `fixVersion in un` → `unreleasedVersions()` appears (version functions).
    5. `sprint in op` → `openSprints()` appears (sprint functions).
    6. Accept `membersOf` from the popup (Tab or Enter) → text becomes `membersOf("")` and the caret lands BETWEEN the quotes ready to type the group name.
    7. Accept `now` → text becomes `now()` and caret lands after the `)`.
  - **Expected**: Each function appears only on a value token for a field of the matching family. Non-matching fields (e.g. `summary = cu`) do NOT show function suggestions.
  - **Plane fallback**: Switch tracker to Plane → confirm Plane filter input shows no function suggestions (Plane engine unchanged).
  - **Pre-fix behaviour**: only `currentUser()` and `membersOf()` were ever suggested, and only for user fields. `now()`, version, and sprint functions had to be typed by hand.

- ⏳ **V22 — Non-system users from the catalog appear in user-field suggestions.**
  - **Setup**: Jira active; let the field-catalog fetch complete (users come in alongside fields).
  - **Action**: In the JQL input, type `assignee = <first letter of a teammate's display name>`.
  - **Expected**: The popup lists matching non-system users (humans). Each row shows `Display Name (email@domain)` and inserts a quoted `"Display Name"` value on accept. Atlassian Connect / Forge app accounts (`accountType == "app"`) and Jira Service Management portal accounts (`accountType == "customer"`) do NOT appear.
  - **Cross-check**: Type `reporter in (` — same user list (reporter is also a user-type field).
  - **Pre-fix behaviour**: only the async-debounced live user search ever returned anything; the catalog fetch's user list was discarded after the fetch completed, so offline / cached suggestions never showed.

---

## Lower priority / one-shot validations

- ⏳ **L1 — Process shutdown smoke under load.** Open the app, trigger a tracker sync, queue a few Lua automation jobs, then quit while sync is in flight. Expected: clean exit, no `std::terminate`, no dangling threads. (Covers PR #10 cumulative effect — workers join before member destruction.)
- ⏳ **L2 — Logger file-sink path rotation.** Change the log-file path while the app is running. Expected: old sink stops cleanly, new sink starts, no lost messages, no double-spawn. (Covers PR #9.)
- ⏳ **L3 — Local cache concurrent writes.** Drive a UI-thread `SaveTicket` while a Lua automation worker also writes to the cache. Expected: no SQLite mis-binding, both writes land. (Covers PR #10 item 5 — `stmtMutex_`.)

---

## How to add a new entry

1. Add a new row in the appropriate section (or create a new section for a new PR).
2. Number M-series for "must smoke before considering the change validated"; L-series for "lower priority / one-shot."
3. Always include: Setup / Action / Expected / Pre-fix behaviour. The pre-fix line is what tells the next reader why this smoke exists.
4. When you run it: flip the emoji, add `(YYYY-MM-DD, initials, one-line note)` after the title.
