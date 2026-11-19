# No test runs both keybinding seed migrations on one load

- **Category**: test
- **Priority**: P3
- **Date**: 2026-11-18
- **Observed on**: PR #2443 (input UX 4.4 action keys), review of its develop merge.
- **Status**: open

## What happened

Two one-time keybinding seed migrations now run in `ConfigManager_Load.cpp`: `MigrateGridSearchFocusKeybindingV1` (`migrated_grid_search_focus_hotkey_v1`, from #2435) and `MigrateActionKeysV1` (`migrated_action_keys_v1`, `preferences.search.focus` / `view.next` / `view.previous`, from #2443). `tests/Core/ConfigMigration.test.cpp` tests each on its own. Nothing loads a config that needs both, so an ordering or flag mix-up between them would not be caught.

## Concrete next action

Add a doctest case to `tests/Core/ConfigMigration.test.cpp`: write a pre-migration config missing all four rows, load it, assert each of `grid.search.focus`, `preferences.search.focus`, `view.next`, `view.previous` appears exactly once and both flags are set; then Save, Load again, and assert no row was seeded a second time.
