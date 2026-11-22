- 2026-11-22 · input-ux-w2-gesture-keys · [debt] · P3 — Gesture keys leftovers: grid column keys, sort-rule list, views row reorder, list popups and palette outside-click

Details:
`docs/plans/input-ux-w2-gesture-keys.md` (Wave 2 piece 4.6, PR #2440) gave drags and resizes a keyboard path.
These were deferred while #2432 and #2434 held their files; both have merged, so they are unblocked:

- **#138 grid column keys:** `grid.column.move_left` / `move_right` / `narrow` / `widen` on the focused pane's
  current column (`SmatchetActiveProjectGridTable.cpp`, `SpreadsheetState.h`).
- **#71:** a filterable Add Sort Rule list on `FilterDropdown`, plus `ui.list.*` on the sort rules
  (`SmatchetGridHeaderUi.cpp`).
- **Views-dashboard row reorder:** rebind its hardcoded Alt+Up / Alt+Down to `ui.list.*`
  (`SmatchetViewsDashboardUi_widgets.cpp`).
- **#119:** move the toolbar command picker, the Keybindings *Add shortcut* popup and the icon picker onto
  `FilterDropdown` (the user's 2026-11-16 decision: no second list-navigation helper).
- **#18 remainder:** an outside click closes the command palette (`CommandPaletteUi.cpp`).
- **`--strict` for the drag check:** `scripts/dev/test-drag-keyboard-equivalent.sh` still lists three unmarked
  drag sources as WARN-only; mark them and turn `--strict` on in CI.
- **Bucket-E mutation check not run** for `tests/ui/gesture_keys.test.cpp`: revert a consumer's take, a command's
  post or a default binding on a scratch branch and confirm each case goes red.

Already tracked elsewhere: column resize on read-only tables
(`debt/2026-11-16-readonly-table-column-resize-keys.md`), the headless edge-panel gap
(`tooling/2026-11-17-bucket-e-headless-lane-realises-no-edge-panel.md`), the WindowExpand dock flake
(`tooling/2026-11-17-bucket-e-windowexpand-dock-flake`); #29 / #30 belong to Wave 3.

Concrete next action:
One PR per bullet (the grid column keys and the sort-rule list touch the grid; the popups move is one PR), each
with a `GestureKeys/*` bucket-E case. Run the mutation check on a scratch branch. Delete each bullet as it lands.

Status: open
Last-reviewed: 2026-11-22
