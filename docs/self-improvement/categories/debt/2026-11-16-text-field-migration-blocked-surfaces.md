- 2026-11-16 · input-ux-w2-text-field · [debt] · P2 — Text-field migration stopped at other branches' plan-locks; its tests sit in neighbouring files

Details:
The shared text field (`Source/Core/include/Ui/SmatchetInput.h`, plan `docs/plans/input-ux-w2-text-field.md`)
shipped with Preferences (Tracker credential rows, templates, Annotate), the icon picker
and the toolbar editor migrated. These surfaces were left on raw `InputText` because their files were
held by another branch's plan-lock when it was written (no lock override was used):

- Grid inline text edit, 512 B buffer, commit refused when the stored value is longer (#36):
  `Source/Core/include/SpreadsheetState.h` (`EditBuffer`) and `Source/Core/src/TicketFieldEditor.cpp`.
- Suggestion popup for fields other than the view JQL editor (#10): `SmatchetViewsDashboardUi_widgets.cpp`.
- Every buffer that lives in `Source/Core/include/Ui/SmatchetUiSession.h` still goes through
  `SmatchetInput::TextBuf` (fixed buffer, byte counter, select-all, trim), not `std::string`: the
  Preferences search box (64 B, #2), the tracker credential buffers, the view name buffer.
- Offline conflict resolver: `conflictResolveBuf` (64 KiB / 8 KiB, #112) is in `SmatchetUiSession.h`.

Tests: the doctest cases for `SmatchetInputPure.h` are in `tests/Core/SmatchetWorklogSubmitPure.test.cpp`
and the bucket-E tests for the text field are in `tests/ui/tracker_first_run_setup.test.cpp` (probe window and
Preferences Email row) and `tests/ui/toolbar_append_cache_cue.test.cpp` (icon picker), because a new test
file needs `tests/CMakeLists.txt`, `tests/ui/CMakeLists.txt` and `tests/ui/ui_tests_registry.cpp`, all locked.

Concrete next action:
When those locks release, move the cases to `tests/Core/SmatchetInputPure.test.cpp`,
`tests/ui/text_field.test.cpp` and `tests/ui/icon_picker.test.cpp` (with their registry entries), then
migrate each surface above to `SmatchetInput::Text` and delete the matching bullet here.

2026-11-20 review: the plan-locks are gone, and the view rename (#5) landed
(`SmatchetViewsDashboardUi.cpp`, input UX Wave 2 lock-deferrals PR). Of what is left, these wait on open PRs:
`tests/CMakeLists.txt`, `SmatchetUiSession.h` and `TicketFieldEditor.cpp` (all three edited by #2452). Since #2440
merged, `tests/ui/CMakeLists.txt`, `tests/ui/ui_tests_registry.cpp` and `SmatchetLocalization.cpp` are free,
so the bucket-E moves and the icon-name message (#125) can go. So can the labels editor (#4), the picker filter
focus (#3) and the suggestion popup (#10): no open PR touches their files.

2026-11-24 review: the labels editor (#4) and the picker filter focus (#3) were already done by the shared dropdown
(#2432: `FilterDropdown.cpp` `FinishEvent` clears the filter after an Add and keeps the caret in a tick-box list;
`tests/ui/filter_dropdown.test.cpp` covers it), and the icon-name message (#125) shipped
(`docs/plans/shipped/input-ux-w2-surface-leftovers.md`); their bullets are gone. `SmatchetUiSession.h` is held by
the `bulk-status-allowed-transitions` plan-lock and `tests/CMakeLists.txt`, `tests/ui/CMakeLists.txt` and
`tests/ui/ui_tests_registry.cpp` by `input-ux-w3-grid-s2`, so the test moves and the session buffers still wait;
the grid inline edit (#36) is Wave 3's (`input-ux-w3-grid-spreadsheet` S3). When `SmatchetUiSession.h` is
free, also move the view-rename start latch (`s_titleWasEditing`, a static in `drawViewsEditorHeader`) onto
`UiDrawSession` beside `viewsTitleEditing` and reset both in `LoadBuffersFromView` (CodeRabbit nitpick on the
lock-deferrals PR #2463).

Status: open
Last-reviewed: 2026-11-24
