# Plan — Focus-aware InputText invalidation for cached Lua cells

> **Slug**: `lua-focus-aware-inputtext` (matches this file's basename without `.md`).
>
> **Status**: `active`.
>
> Graduates item **C1** from the [v2 follow-up stub](docs/plans/lua-recorded-cmd-list-v2.md) into a scoped plan. Closes the one documented data-loss limitation of the shipped [recorded-cmd-list feature](docs/plans/lua-recorded-cmd-list.md) (PR #66, `5b740e9`) — see its § Known limitations row ("InputText buffer reset when `rawValue` changes mid-edit").

## Context

A cached Lua cell is re-recorded whenever any of its cache-key inputs changes — `rawValue`, `fieldName`, `intAvailWidth`, `isReadOnly`, `providerGen` (`TryRenderCachedLuaField`, `AppController_LuaBindings_Draw.cpp:593`). Re-recording rebuilds the cell's `ImCmd` list, and for an `InputText` op that means a fresh `textBuf` primed from the *new* `rawValue` (`LuaDrawList::InputText`, `:248`; the buffer is what ImGui reads/writes during editing, `ReplayInputText`, `:425`).

So if a background sync flips a ticket's `rawValue` **while the user is typing into that cell's cached `InputText`**, the next frame re-records the cell and replaces the buffer mid-edit — the user's in-progress keystrokes vanish. The v1 doc ships this as an accepted limitation with the explicit v2 note: "track focused-cell state and skip re-record while focused."

**Intended outcome — after this lands:** while a cached `InputText` cell owns ImGui keyboard focus, a `rawValue`-only change does not re-record it; the in-progress edit survives. Re-record is deferred until focus drops, at which point the cell picks up the synced value.

## Approach

Two pieces: **detect** that a specific cell owns an active `InputText`, and **gate** the re-record on that.

**Detect** — during replay, `ReplayInputText` already runs `ImGui::InputText`; add an `ImGui::IsItemActive()` check. When true, stamp the owning cell's key + the current ImGui frame number onto UI-thread-only state (`luaActiveInputCellKey_`, `luaActiveInputCellFrame_` on `AppController::Impl`, alongside `luaFieldCache_`). The cell's identity is already threaded into replay as `cbArg1 = ticket.id`, `cbArg2 = fieldId`, so no new plumbing to know *which* cell. Only the cell path stamps this (a `trackInputFocus` flag on the replay context, `false` for windows) so window inputs can't pollute the cell key.

**Gate** — in `TryRenderCachedLuaField`, when the cached entry exists but `inputsMatch` is false, check whether the *only* differing input is `rawValue` (fieldName / width / readOnly / providerGen all equal) **and** this cell currently owns focus (`luaActiveInputCellKey_ == key && luaActiveInputCellFrame_ >= ImGui::GetFrameCount() - 1`). If so, skip the record: replay the existing cached `cmds` and return, leaving `entry.rawValue` at the old value. Because the entry keeps the stale `rawValue`, it keeps hitting this skip branch every frame while focus holds — then the moment focus drops the key no longer matches, the skip condition fails, and the normal miss path re-records with the synced value. The one-frame `GetFrameCount()` staleness bound means a cell scrolled out of view (no longer replayed, so its frame stamp goes stale) stops being treated as focused and re-records when next visible.

The focus signal is read on frame N from the stamp written during frame N-1's replay. That one-frame lag is harmless: keyboard focus persists across frames, so a still-focused input re-stamps every frame and the stamp never goes stale while focus holds.

## Files to modify

1. `Source/Core/src/AppControllerImpl.h` — add to `AppController::Impl` (behind `SMATCHET_WITH_LUA_AUTOMATION`, next to `luaFieldCache_`): `std::string luaActiveInputCellKey_;` and `std::uint64_t luaActiveInputCellFrame_ = 0;`. UI-thread-only, no atomics (mirrors `luaFieldCache_`'s threading note).
2. `Source/Core/include/AppController.h` — declare a public UI-thread method `void MarkLuaInputCellActive(const std::string& issueId, const std::string& fieldId);` (guarded) so the replay free-function can stamp `Impl` state without touching `impl_` directly.
3. `Source/Core/src/AppController_LuaBindings_Draw.cpp` — four touch points:
   - `MarkLuaInputCellActive` body — build `key = issueId + '\0' + fieldId`, store it + `ImGui::GetFrameCount()` into the two `Impl` fields.
   - `ReplayCtx` (`:344`) — add `bool trackInputFocus;`.
   - `ReplayInputText` (`:415`) — after the `ImGui::InputText` call, `if (ctx.trackInputFocus && ImGui::IsItemActive()) ctx.app.MarkLuaInputCellActive(ctx.cbArg1, ctx.cbArg2);`.
   - `ReplayCmdList` (`:486`) — add a `bool trackInputFocus` parameter, forwarded into `ReplayCtx`. Cell call site (`:599`, `:643`) passes `true`; window call site (`DrawLuaWindows:1230`) passes `false`.
   - `TryRenderCachedLuaField` (`:591`) — in the `cit != end()` branch, when `!inputsMatch`, compute a `rawValueOnly` diff and consult the focus stamp; on match, `ReplayCmdList(entry.cmds, *this, lua, ticket.id, fieldId, entry.isReadOnly, /*trackInputFocus=*/true); return entry.handled;` (skip the record block entirely).
4. `LUA_GUIDE.md` — update the `input_text` row / limitations note: in-progress edits are now preserved across background syncs; commit still fires on focus loss and picks up the synced baseline only after the edit ends.
5. `docs/plans/lua-recorded-cmd-list-v2.md` — mark C1 graduated (link here).

## Design (gate condition)

```cpp
// inside the `cit != luaFieldCache_.end()` branch, when !inputsMatch:
const bool onlyRawValueDiffers =
    entry.rawValue    != rawValue    &&   // the one field allowed to differ
    entry.fieldName   == fieldName   &&
    entry.intAvailWidth == intAvailWidth &&
    entry.isReadOnly  == isReadOnly  &&
    entry.providerGen == curProviderGen;

std::string key = ticket.id; key.push_back('\0'); key.append(fieldId);   // already built above
const bool ownsFocus =
    impl_->luaActiveInputCellKey_ == key &&
    impl_->luaActiveInputCellFrame_ + 1 >= static_cast<std::uint64_t>(ImGui::GetFrameCount());

if (onlyRawValueDiffers && ownsFocus && entry.handled) {
    // Defer re-record: keep the in-progress buffer, replay the cached list.
    // entry.rawValue stays stale on purpose → re-records automatically once focus drops.
    ReplayCmdList(entry.cmds, *this, lua, ticket.id, fieldId, entry.isReadOnly, /*trackInputFocus=*/true);
    return true;
}
```

Everything else in `TryRenderCachedLuaField` is unchanged: a non-`rawValue` change (width, read-only, provider churn) records immediately even mid-edit, because those change layout/validity and staleness there is worse than a lost keystroke. `GetFrameCount()` comparison uses `+ 1 >=` to admit the previous frame's stamp; a cell not replayed last frame (scrolled off) fails it and re-records normally.

**Commit interaction**: focus-drop on a real edit fires `IsItemDeactivatedAfterEdit` → the `on_commit` callback runs with the *typed* value (`ReplayInputText:433`) **before** the deferred re-record on the following frame. So the user's typing is committed, not discarded; the subsequent re-record then reflects whatever `rawValue` the commit produced. A focus-drop with no edit fires no commit and the re-record simply adopts the synced value — matching the stub's "draft preserved until explicitly ended" intent.

## Existing utilities reused

- `ReplayCmdList` / `ReplayInputText` / `ReplayCtx` — `AppController_LuaBindings_Draw.cpp:486,415,344` — the existing replay path; the plan threads one bool and one `IsItemActive` check through them.
- `TryRenderCachedLuaField` cache-key comparison — `:593` — the `inputsMatch` fields are reused verbatim to compute the `onlyRawValueDiffers` refinement.
- `ImGui::IsItemActive()` / `ImGui::GetFrameCount()` — stock ImGui; `IsItemActive` is the canonical "this widget owns keyboard/edit focus this frame" query, avoiding the `GetActiveID()`→key mapping the stub worried about (we stamp from inside replay where the cell key is in hand, so no reverse lookup is needed).

## Extraction sizing

N/A — adds two fields, one method, one bool param; extracts nothing.

## UX Pillar callouts

- **Pillar 1 (perf)**: one `IsItemActive()` call per `InputText` replay (only cells) + a handful of string/int compares in the cell miss path. `MarkLuaInputCellActive` builds one key string only when an input is actually active (≤1 per frame). Negligible; the common no-focus path is unchanged.
- **Pillar 2 (UI-thread never blocks)**: no I/O, no new blocking; pure UI-thread state.
- **Pillar 3 (never crash)**: pure comparisons and a replay of an already-valid cached list; introduces no new throw or unbounded allocation. The skip path is strictly a subset of the existing replay path.
- **Pillar 4 (accessibility)**: direct improvement — eliminates silent data loss mid-edit, which is a real keyboard-user hazard. No nav/scaling change.

## Perf-review-system gates

Diff touches `Source/Core/` → gates fire.

1. **PR-fast CI** — `priority-grid-scroll` is the closest existing cell-replay scenario for regression coverage. Add a focused bucket-E test (below) for the new behaviour; a full scenario is optional since the change is a branch, not a new hot path.
2. **Pillar 2 static scanner** — no new sync-I/O reachable from `ImGui::*`. N/A.
3. **Dispatcher drain** — untouched. N/A.
4. **Visible-cue bucket-E harness** — no new >100 ms path. N/A.
5. **Marker inventory** — no new `SMATCHET_UI_PERF_SCOPE` markers. N/A.

## Risks / non-goals

- **False "owns focus" across cell reuse** — if the same `(ticket.id, fieldId)` key is legitimately a different rendered cell (grid virtualisation reusing rows), the one-frame `GetFrameCount()` bound plus the fact that focus is a global single-widget property in ImGui makes a wrong match effectively impossible (only one widget is active at a time, and its key is stamped fresh each frame). Accepted.
- **Window vs cell key collision** — a window `InputText` being active would stamp `windowName + '\0' + ""`; suppressed by passing `trackInputFocus = false` from the window replay path, so windows never write the cell key. Enforced, not merely improbable.
- **Deferred value never adopted if focus never drops** — a cell edited and left focused forever keeps replaying the pre-sync list. Correct by design: the user is actively editing; adopting a background value under the cursor is the bug we're fixing.
- **Non-goal**: preserving edits across a `providerGen` bump or a width/read-only change. Those legitimately rebuild the cell; only `rawValue`-only churn is deferred.
- **Non-goal**: window `InputText` focus preservation. Windows re-record as a unit on gen bump; a per-op focus carve-out there is a separate, larger change (out of scope, noted below).

## Verification

- **Bucket A (pure-logic ctest)**: extract the gate decision — inputs `(rawValueDiffers, otherInputsEqual, ownsFocus, handled)` → `{skip-record, replay} | {fall through to record}` — into a pure predicate and table-test it, including the frame-staleness boundary (`frame`, `frame-1`, `frame-2`).
- **Bucket E (ImGui Test Engine)**: register a cached provider that draws `draw:input_text(...)`; focus the cell, type text, then drive a `rawValue` change through a simulated background sync (`UpdateTicket` on that ticket from another source); assert (a) the typed text is still in the buffer, (b) the cell's record count did **not** increment; then blur the cell and assert it re-records and shows the synced value. Add a second case: a *width* change mid-focus **does** re-record (negative control).
- **Bash-driver scenario**: extend `lua-recorder-fuzz` (or a focused variant) to include a "sync-during-focus" step so the fuzzer exercises the new branch.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target; new `Impl` fields + method are behind `SMATCHET_WITH_LUA_AUTOMATION`, so confirm the `-DSMATCHET_WITH_LUA_AUTOMATION=OFF` stub build still links).
- **Doc validation**: `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs`**: run before finalising; record outcome.
- **Manual residue**: none intended.

## Out of scope (flagged, not designed)

**Deferral residue-sweep** — on finalising, update the C1 reference in `docs/plans/lua-recorded-cmd-list-v2.md` (graduated) and the § Known limitations row in `docs/plans/lua-recorded-cmd-list.md` if that shipped doc is being revised in the same PR.

- **Window `InputText` focus preservation** — windows re-record wholesale on gen bump; carving a focused op out of a window re-record is a larger change and is not addressed here.
- **Multi-widget focus (combo/drag with open popups)** — those ops don't exist yet (stub A1); when added, each needs its own "is-editing" predicate. Not designed.
- **Preserving selection/caret position, not just buffer contents** — this plan preserves the buffer; caret/selection state is ImGui-internal and unaffected by the skip (the same widget persists across frames), so no extra work — but explicitly not a guarantee we test.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip § Status to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.
