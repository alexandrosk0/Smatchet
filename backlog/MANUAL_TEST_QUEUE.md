# Manual test queue

> Smoke tests that aren't covered by any PR's automated build verification. Each entry is something a human has to actually click / run / observe at a keyboard. Tests are grouped by the PR that introduced the behaviour they validate.
>
> When you complete a smoke, flip ⏳ → ✅ in place and add a one-line note (date + result + tester initials). If a smoke regresses, flip to ❌ and file a new backlog item.

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
  - **Command**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`
  - **Expected**: 0 errors, 0 new warnings vs the develop tip pre-cleanup (last clean baseline: `93f561b`).
  - **Why it matters**: PRs #12 / #15 / #16 each rebased against a moving develop; the post-merge tip was never validated as a single unit.

- ⏳ **B2 — DX12 ImGui host links clean** (only if #12's item 22 is being smoked).
  - **Command**: `cmake --build --preset ninja-iter-msys2 --target SmatchetImGuiHost_DX12`
  - **Expected**: clean build of the DX12 host static lib.

---

## From [PR #12](https://github.com/alexandrosk0/Smatchet/pull/12) — P1 cleanup batch

Four runtime fixes, each needs an independent smoke.

- ⏳ **M1 — MCP SSE heartbeat shutdown latency (item 9).**
  - **Setup**: Build standalone; enable MCP server (`Mcp` plugin started). Connect one (or several) SSE clients to `/mcp/sse`. The MCP Inspector tool or a `curl -N http://localhost:<port>/mcp/sse` works.
  - **Action**: While clients are connected, quit Smatchet (window close or `Alt+F4`).
  - **Expected**: Process exits within ~100 ms of the quit. Watch the OS process list — `SmatchetStandalone.exe` should be gone almost immediately, not lingering for 1+ second per connected client.
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
