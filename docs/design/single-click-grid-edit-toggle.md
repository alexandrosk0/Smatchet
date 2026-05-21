# Plan — single-click / double-click toggle for grid cell edit

> Slug: **`single-click-grid-edit-toggle`** (kebab-case, matches the feature).

## Context

Smatchet's editable grid cells today have **inconsistent click-to-edit semantics** across field types:

| Field type | Today's edit trigger | Code |
|---|---|---|
| Text | Double-click on `Selectable` preview | [`TicketFieldEditor.cpp:551-554`](../../Source_Core/src/TicketFieldEditor.cpp) |
| Single-select | **Single-click** opens `BeginCombo` immediately | [`TicketFieldEditor.cpp:594`](../../Source_Core/src/TicketFieldEditor.cpp) |
| Multi-select | **Single-click** opens `BeginCombo` immediately | [`TicketFieldEditor.cpp:703`](../../Source_Core/src/TicketFieldEditor.cpp) |
| Cascading select | **Single-click** opens `BeginCombo` immediately | [`TicketFieldEditor.cpp:791`](../../Source_Core/src/TicketFieldEditor.cpp) |
| Labels | **Single-click** opens `BeginCombo` immediately | [`TrackerLabelsEditor.cpp:68`](../../Source_Core/src/TrackerLabelsEditor.cpp) |
| Date / DateTime | Double-click when populated; **single-click when blank** | [`TrackerDateTimeFieldEditor.cpp:347-349`](../../Source_Core/src/TrackerDateTimeFieldEditor.cpp) |

Goal: one new preference unifies every editable cell. User-confirmed scope = **all editable fields** (text + single-select + multi-select + cascading + labels + date/datetime). User-confirmed default = **single-click to edit**.

Non-editable cells unaffected (ID column's double-click → `OpenUrl` at [`SmatchetActiveProjectGridUi.cpp:848`](../../Source_Core/src/SmatchetActiveProjectGridUi.cpp) is non-editable, no collision either way).

Selection (rect-sel + shift/ctrl modifiers via `handleCellRectSel` at [`SmatchetActiveProjectGridUi.cpp:660-700`](../../Source_Core/src/SmatchetActiveProjectGridUi.cpp)) reads `IsMouseClicked` independently of the cell's own click handler, so selection coexists with edit-start in **both** modes — matches today's combo-cell UX (clicking a select cell selects the row AND opens the picker simultaneously).

## Approach

**One bool** in `TrackerConfig` → threaded through the single dispatcher `TicketFieldEditor::RenderFieldCell` → passed to each per-type editor function. **One new key** in `SpreadsheetState` (`EditArmedKey`) lets the double-click path arm an editor on frame N + render the popup on frame N+1.

Per-field-type behaviour in each mode:

| Field type | Single-click mode (default) | Double-click mode |
|---|---|---|
| Text | Drop `IsMouseDoubleClicked` guard at `TicketFieldEditor.cpp:551-554` — `Selectable` click directly calls `state.StartEditingField` | Keep current behaviour |
| Date / DateTime | Drop `IsMouseDoubleClicked` guard at `TrackerDateTimeFieldEditor.cpp:347-349` — `Selectable` click directly opens picker (`OpenPopup("picker")`) | Keep current behaviour |
| Single-select / multi-select / cascading / labels | Keep current `BeginCombo` (already single-click) | Replace direct `BeginCombo` with `Selectable` preview; on double-click set `state.EditArmedKey = ticket.id + "::" + field.Id`; next frame, if `EditArmedKey` matches this cell, render `BeginCombo` + `ImGui::OpenPopup` once + clear `EditArmedKey` on combo close |

Single-click mode is the cheap path — code-wise it just deletes the double-click guards on text + date. Double-click mode is the new code path — adds the arm-then-popup state machine to the four combo-based editors.

## Files to modify

1. **`Source_Core/include/ConfigManager.h:79`** — add `bool SingleClickToEditGridCells = true;` alongside `EnableFieldOverflowTooltips`. Comment: `// When true (default), single click on a grid cell starts editing. False requires double-click. Exposed in Settings -> Preferences -> Appearance.`
2. **`Source_Core/src/ConfigManager.cpp:185`** — add `j["single_click_to_edit_grid_cells"] = config.SingleClickToEditGridCells;` next to the existing `j["field_overflow_tooltips"]` line.
3. **`Source_Core/src/ConfigManager.cpp:656`** — add `cfg.SingleClickToEditGridCells = j.value("single_click_to_edit_grid_cells", cfg.SingleClickToEditGridCells);` next to the existing `EnableFieldOverflowTooltips` load line. **No schema-version bump** — additive boolean defaulted via `j.value()`, matches existing convention for every recent bool added (e.g. `EnableFieldOverflowTooltips`, Whisper / Agentic flags).
4. **`Source_Core/include/SpreadsheetState.h`** — add `std::string EditArmedKey;` alongside `SingleSelectActiveKey` / `MultiSelectActiveKey`. Cleared on combo close + on ticket-grid pivot.
5. **`Source_Core/include/TicketFieldEditor.h`** — extend `RenderFieldCell` signature with `bool singleClickToEdit`. Place at the end (matches existing parameter order — feature flags after config + flags `tooltipsEnabled`, `allowEdits`).
6. **`Source_Core/src/TicketFieldEditor.cpp`** — five edits:
   - `RenderFieldCell` accepts + forwards `singleClickToEdit` to each per-type renderer (text + single-select + multi-select + cascading + date).
   - `RenderTextEditor` (line 480) — wrap the `IsMouseDoubleClicked` guard at line 552 with `if (singleClickToEdit || ImGui::IsMouseDoubleClicked(0))`.
   - `RenderSingleSelectEditor` (line 569) — in double-click mode, render `Selectable` preview first; arm `EditArmedKey` on double-click; only call `BeginCombo` + `OpenPopup("##singleselect")` when this cell is armed. In single-click mode, unchanged.
   - `RenderMultiSelectEditor` (line 694) — same shape as single-select.
   - `RenderCascadingSelectEditor` (line 780) — same shape.
7. **`Source_Core/src/TrackerLabelsEditor.cpp:49`** — extend `RenderLabelsFieldEditor` signature with `bool singleClickToEdit` + `SpreadsheetState& state`. Same arm-then-popup pattern as the four select editors. Update header `TrackerLabelsEditor.h` + caller in `RenderFieldCell` (line 999).
8. **`Source_Core/src/TrackerDateTimeFieldEditor.cpp:347-349`** — extend `RenderDateTimeFieldEditor` signature with `bool singleClickToEdit`. Replace the `blankValue || IsMouseDoubleClicked(0)` with `blankValue || singleClickToEdit || IsMouseDoubleClicked(0)`. Update header + caller at `TicketFieldEditor.cpp:1031`.
9. **`Source_Core/src/SmatchetActiveProjectGridUi.cpp:923-926`** — pass `d.cfg.SingleClickToEditGridCells` to `TicketFieldEditor::RenderFieldCell`.
10. **`Source_Core/src/SmatchetPreferencesUi.cpp:2179`** — add checkbox under "Grid and field text" subsection, modelled exactly on the `EnableFieldOverflowTooltips` checkbox. Label: `"Single-click to edit grid cells"`. Tooltip: `"When off, double-click is required to begin editing any cell. Default: on."`. Uses `MarkPrefsDirty(d)` on change (matches the existing pattern — preferences UI does NOT call `ConfigManager::Save` directly).
11. **`Source_Core/src/Commands/Builtin/BuiltinCommands_Config.cpp:101`** — add `{"singleClickToEditGridCells", "single_click_to_edit_grid_cells", ""},` to the `kKeys[]` table, alongside `enableFieldOverflowTooltips`. Auto-wires `config.set singleClickToEditGridCells false` over Command Palette / MCP / Lua.

## Existing utilities reused (no new API)

- `SpreadsheetState::StartEditingField(...)` — canonical text edit-start.
- `SingleSelectActiveKey` / `MultiSelectActiveKey` keying convention (`ticket.id + "::" + field.Id`) — `EditArmedKey` mirrors it.
- `ConfigManager::Save/Load` with `j.value(...)` defaulting — matches every other bool config field.
- `MarkPrefsDirty(d)` in `SmatchetPreferencesUi.cpp` — pref-change persistence hook.
- `BuiltinCommands_Config` `kKeys[]` declarative table — auto-wires `config.set` / `config.get` for the new key.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: `RenderFieldCell` is in the per-cell hot path. Change adds at most one bool comparison per cell — no allocations, no new branches in steady-state inner loop. No baseline bump expected.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — no new sync I/O; preference is a struct field read.
- **Pillar 3 (never crash)**: `EditArmedKey` is a `std::string`, default-constructed, written under existing UI-thread guarantees. No new heap ops, no nullptr deref.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no impact — change is mouse-gesture only; keyboard-nav path through `Ctrl+Shift+P` Command Palette → `config.set singleClickToEditGridCells …` remains intact.

## Perf-review-system gates (per `docs/design/pillar-1-2-perf-review-system.md`)

Active gates on this PR (no opt-in needed):

1. **PR-fast CI** (`.github/workflows/perf-pr-fast.yml`) — auto-runs `idle`, `priority-grid-scroll`, `command-palette-fuzzy`, `cell-edit-burst` against `docs/perf/baselines/<scenario>.ci-windows-latest.json`. **`priority-grid-scroll` directly exercises `RenderFieldCell`** per `agents/perf-gatekeeper.md` § Curated diff → scenario map → load-bearing gate for this feature.
2. **Pillar 2 static scanner** (`scripts/dev/pillar2-scan.sh` via end-of-turn lint drain) — runs on every edited `.cpp`/`.h`. Expected clean: no new `cpr::` / `SQLite::` / `std::ifstream` / `std::mutex::lock` introduced.

**Pre-push local check**: run `docs/PERF_WORKFLOW.md` § Gate-check vs baseline (Step 7) against `priority-grid-scroll` before opening the PR.

**Optional but recommended**: invoke `perf-gatekeeper` agent pre-PR — picks `priority-grid-scroll` automatically + posts delta.

**Override**: `perf-out-of-band` PR label per `AGENTS.md` § Merge gates — **do not apply** here; no reason to expect a regression. If `priority-grid-scroll` regresses, root-cause + fix rather than label-around.

**Doesn't apply**: dispatcher drain (we don't touch `MainThreadDispatcher`), visible-cue harness (we add no sync stall), marker inventory (we add no `SMATCHET_UI_PERF_SCOPE` markers).

## Risks / non-goals

- **Drag-select on text cells**: in single-click mode, `MouseDown` on a text cell focuses `InputText` (auto-focused by `ImGui::SetKeyboardFocusHere`). Rect-sel drag still works (separate `IsMouseClicked` hit-test in `handleCellRectSel`), but the user sees a brief flash of the editor before drag begins. **Same UX trade-off select-type cells make today** — accepted, not blocked.
- **Modifier-bypass for edit start**: out-of-scope. If the user wants "shift-click selects without editing", that's a follow-up issue — flag in `docs/backlog/agent-self-improvement/` (category `process`) only if a real user reports it.
- **Esc / focus-loss commit-or-cancel**: unchanged — handled by existing `StartEditingField` / `BeginCombo` flows.
- **DX12 / Unreal**: `Source_Core/` change compiles into both targets. Verify via `--target SmatchetStandalone SmatchetCore_DX12`. No `glClearColor` / `GLFW` touch.

## Verification

**Bucket A (pure-logic ctest, `test-rig`)** — `tests/Source_Core/SingleClickEditConfig.test.cpp`:
- Round-trip `SingleClickToEditGridCells = false` via `ConfigManager::Save` → `Load` → assert false survives.
- Round-trip with key absent from JSON → assert loaded value is `true` (default).
- `config.set singleClickToEditGridCells false` via the registered command → assert struct field flips + `config.get singleClickToEditGridCells` returns `false`.

**Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msys2`)** — `tests/ui/grid_click_edit_mode.test.cpp`:
- Open ticket grid with one text + one single-select + one date column.
- With default config: single-click each cell → assert editor active (text input focused; combo open; date picker open).
- Flip config via `config.set` command: single-click each cell → assert NO editor; double-click → assert editor active.
- Coverage residue (label / multi-select / cascading) — defer to one of: bucket-E follow-up if reuse cost is low, or `docs/backlog/agent-self-improvement/test.md` entry naming the gap explicitly. No silent residue.

**Build gate**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — dual-target must compile.

**Manual visual-validation pause-loop** (per `AGENTS.md` § Visual-validation exception — diff touches `Source_Core/src/Smatchet*Ui*.cpp` AND new bucket-E scenario doesn't yet cover label/cascading): pause the ship-loop after build, launch `build/ninja-iter-msys2/Smatchet.exe`, verify (1) checkbox visible in Preferences → Appearance, (2) default = on lets text-field edit on single click, (3) toggle off requires double-click on text + select + multi-select + date + cascading + labels. User commits the verdict before push.

## Out of scope (flagged, not designed)

- Touch / pen / mobile gestures — Smatchet is desktop-only today.
- Keyboard-only edit-trigger (e.g. F2 to edit) — separate keyboard-nav slice; flag in `docs/backlog/agent-self-improvement/process.md` if a real user asks.
- Per-column override of the click mode — adds N×M config keys; defer until evidence the global toggle is insufficient.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
