# No test runs the keybinding seed migrations together on one load

- **Category**: test
- **Priority**: P3
- **Date**: 2026-11-18
- **Observed on**: PR #2443 (input UX 4.4 action keys), review of its develop merge; PR #2440 (input UX 4.6 gesture keys), review of its develop merge.
- **Status**: open

## What happened

Three one-time keybinding seed migrations now run in `ConfigManager_Load.cpp`: `MigrateGridSearchFocusKeybindingV1` (`migrated_grid_search_focus_hotkey_v1`, from #2435) and `MigrateActionKeysV1` (`migrated_action_keys_v1`, `preferences.search.focus` / `view.next` / `view.previous`, from #2443). A third, `MigrateGestureKeybindingsV1` (`migrated_gesture_keys_v1`, the seven `ui.list.*` / `ui.splitter.*` / `view.panel.*` rows, from #2440), runs first: `LoadListFields` calls the gesture seed, then the grid-search seed, then (after the zoom passes) the action-keys seed. `tests/Core/ConfigMigration.test.cpp` tests each on its own. Nothing loads a config that needs all three, so an ordering or flag mix-up between them would not be caught.

## Concrete next action

Add a doctest case to `tests/Core/ConfigMigration.test.cpp`: write a pre-migration config missing all eleven rows, load it, assert each of `grid.search.focus`, `preferences.search.focus`, `view.next`, `view.previous` and the seven gesture rows (`ui.list.move_up`, `ui.list.move_down`, `ui.list.remove`, `ui.splitter.grow`, `ui.splitter.shrink`, `view.panel.grow`, `view.panel.shrink`) appears exactly once and all three flags are set; then Save, Load again, and assert no row was seeded a second time.
