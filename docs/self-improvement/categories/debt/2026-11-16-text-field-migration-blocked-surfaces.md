- 2026-11-16 · input-ux-w2-text-field · [debt] · P2 — Text-field migration stopped at other branches' plan-locks; its tests sit in neighbouring files

Details:
The shared text field (`Source/Core/include/Ui/SmatchetInput.h`, plan `docs/plans/input-ux-w2-text-field.md`)
shipped with Preferences (Tracker credential rows, templates, Annotate), the icon picker, the view rename
and the toolbar editor migrated. These surfaces were left on raw `InputText` because their files were
held by another branch's plan-lock when it was written (no lock override was used):

- Grid inline text edit, 512 B buffer, commit refused when the stored value is longer (#36):
  `Source/Core/include/SpreadsheetState.h` (`EditBuffer`) and `Source/Core/src/TicketFieldEditor.cpp`.
- Labels editor: Enter adds a label and drops the caret (#4): `Source/Core/src/Tracker/TrackerLabelsEditor.cpp`.
- Filter box focus in the project picker and labels popup (#3): `SmatchetProjectPicker.cpp`,
  `TrackerLabelsEditor.cpp`.
- Suggestion popup for fields other than the view JQL editor (#10): `SmatchetViewsDashboardUi_widgets.cpp`.
- Every buffer that lives in `Source/Core/include/Ui/SmatchetUiSession.h` still goes through
  `SmatchetInput::TextBuf` (fixed buffer, byte counter, select-all, trim), not `std::string`: the
  Preferences search box (64 B, #2), the tracker credential buffers, the view name buffer.
- Offline conflict resolver: `conflictResolveBuf` (64 KiB / 8 KiB, #112) is in `SmatchetUiSession.h`.
- Toolbar editor icon name "No such icon" message (#125) needs a new localization row in
  `SmatchetLocalization.cpp`, which was held by three branches.

Tests: the doctest cases for `SmatchetInputPure.h` are in `tests/Core/SmatchetWorklogSubmitPure.test.cpp`
and the bucket-E tests for the text field are in `tests/ui/tracker_first_run_setup.test.cpp` (probe window and
Preferences Email row) and `tests/ui/toolbar_append_cache_cue.test.cpp` (icon picker), because a new test
file needs `tests/CMakeLists.txt`, `tests/ui/CMakeLists.txt` and `tests/ui/ui_tests_registry.cpp`, all locked.

Concrete next action:
When those locks release, move the cases to `tests/Core/SmatchetInputPure.test.cpp`,
`tests/ui/text_field.test.cpp` and `tests/ui/icon_picker.test.cpp` (with their registry entries), then
migrate each surface above to `SmatchetInput::Text` and delete the matching bullet here.

Status: open
Last-reviewed: 2026-11-16
