# The LeftViewHistory bucket-E test does not run in CI

- **Category**: test
- **Priority**: P1
- **Date**: 2026-11-09
- **Observed on**: the left-view history grid PR (`docs/plans/active/left-view-history-grid.md`, § Deviations).
- **Status**: open

## What happened

`tests/ui/left_view_history.test.cpp` only runs with its fixture (`scripts/dev/test-ui-left-view-history.sh` sets `tests/fixtures/jira_backend/left-view-history.json`). The `bucket-e-ui-tests` `--all` lane has no fixture, so the test logs SKIP and passes green. The end-to-end check (sync → listing diff → SQLite → reason → window) therefore gates no merge. The wiring was written and reviewed, then dropped from the PR because plan-lock `clang-cl-ubsan-lanes` (branch `fix/clang-cl-ubsan`) holds `.github/workflows/build-and-test.yml`.

## Concrete next action

Once `fix/clang-cl-ubsan` merges (or its lock is released), add `test-ui-left-view-history` to the `for driver in test-ui-grid-search-apply test-ui-grid-pane-windows; do` loop of the "Run grid search + grid panes bucket-E (hard)" step in `build-and-test.yml`, and name the group in that step's comment. The wrapper overrides the loop's `basic-grid.json` fixture with its own, so no other env change is needed.
