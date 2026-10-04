- 2026-10-03 · deviation renewal (wrapped-marker sweep) · [debt] · P3 — two UI translation units sit well past the 1,200-line advisory ceiling

Details:
`Source/Core/src/Ui/SmatchetUI.cpp` (1,628 lines) and `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp`
(1,351 lines) are the only first-party TUs that silence `tu-line-ceiling` with a marker. Both markers used
to be wrapped across lines and carried slug revisits ("next dedicated SmatchetUI.cpp file-split pass",
"next touch of this file") that nothing enforced. The Views file has been touched by several PRs since
its marker said "next touch", and has grown from about 15 lines over the ceiling to 151. Both markers
are now one line with dated revisits (`SmatchetViewsDashboardUi.cpp` 2027-04-30, `SmatchetUI.cpp`
2027-06-30), so `deviation-overdue` brings them back.

Concrete next action:
- `SmatchetViewsDashboardUi.cpp`: move the view actions (`viewsApplyAndSync`, `viewsDiscardChanges`,
  `viewsActivateView`, `viewsRequestActivate`, `viewsCreateNewView`, `applyPendingViewCreate`,
  `applyPendingViewDelete`, `handleViewsDashboardShortcuts`) into a companion
  `SmatchetViewsDashboardUi_actions.cpp`. Keep the draw functions where they are, next to the existing
  `_widgets.cpp` / `_detail.h` split. Delete the marker.
- `SmatchetUI.cpp`: move the app-update check and modal (`StartAppUpdateCheck`, `DrainAppUpdateCheck`,
  `DrawAppUpdateModal`) and the keybinding block (`rebuildKeybindingCache`, `dispatchKeybindings`,
  `drawQuickBindPopup`) into companion TUs beside `SmatchetUI_Layout.cpp` / `SmatchetUI_MainMenu.cpp`.
  Delete the marker once the TU is under 1,200 lines.
- Both are moves only: no behaviour change, verified by the bucket-E UI lanes and the bucket-C captures.

Status: open
Last-reviewed: 2026-10-03
