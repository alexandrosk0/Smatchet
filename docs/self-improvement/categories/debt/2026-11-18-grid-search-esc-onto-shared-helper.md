# The grid search box still inlines the Esc-clears-search block

- **Category**: debt
- **Priority**: P3
- **Date**: 2026-11-18
- **Observed on**: PR #2443 (input UX 4.4 action keys), DRY gate after its develop merge.
- **Status**: open

## What happened

The settings search (`SmatchetPreferencesUi.cpp`, #2443) and the grid search (`SmatchetGridHeaderUi.cpp`, #2435) each had the same block that makes Esc clear the query and leave the box (clear the buffer, `ClearActiveID`, drop `InputTextDeactivatedState`). #2443 moved it to `SmatchetInput::ClearSearchOnEscape` and the settings search calls it, but `SmatchetGridHeaderUi.cpp` was held by plan-lock `pane-picker-jira-sites`, so the grid copy stayed inline and the helper carries a `SMATCHET_DEVIATION(rule=duplication)` marker (revisit 2027-01-31).

## Concrete next action

Once `pane-picker-jira-sites` is released, replace the inline block before `##GridFilter` in `SmatchetGridHeaderUi.cpp` with `SmatchetInput::ClearSearchOnEscape(pane.gridSearchInputId, pane.gridSearchBuf);` (add `#include "Ui/SmatchetInput.h"` if missing), then delete the deviation marker above `ClearSearchOnEscape` in `Source/Core/src/Ui/SmatchetInput.cpp`. The `GridSearch` bucket-E tests cover the Esc behaviour.
