- 2026-11-23 · orchestrator · [test] · P2 — No bucket-E test drives the grid's catalog read-only cue and its Retry button
  Details: When a grid pane's columns resolve no field in its catalog, `NoteAndCueCatalogReadOnly` (`Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp`) draws a cue above the table, with a tooltip. It offers Retry (`DrawCatalogCueRetry`) for the Unavailable cue, and for NotLoaded once the pane has a stored catalog request (`PaneCatalogFetchStatus::CanRetry`: a cold-start pane whose catalog fetch never ran has none, so it shows no Retry), and logs each cue change from `LogCatalogCueChange`. Those log lines start `Grid pane='<id>' cells are read-only for a catalog reason:`, followed by `field catalog still not available (` for a repeat of the same failure and `cells are editable again.` when the pane recovers. The cue decision (`ChooseGridCatalogCue`), the coverage helpers and the controller status (`GetPaneCatalogFetchStatus`, `IsGridCatalogFetchForPane`) have doctest coverage. Nothing covers the drawn part:
  - the cue text and tooltip appear over a drawn table and never over the welcome, loading or error states;
  - Retry appears only when the fetch can run again, and clicking it calls `RetryPaneFieldCatalog`;
  - Retry moves to its own line in a narrow pane instead of being clipped;
  - one log line is written per cue change.
  So the test plan of alexandrosk0/Smatchet#2481 keeps two manual checks: a visual sign-off and a Windows app run that looks for those log lines.
  Concrete next action: Add a bucket-E test under `tests/ui/`, registered in `tests/ui/ui_tests_registry.cpp` and `tests/ui/CMakeLists.txt`. It drives one grid pane, through the scenario seam the other `tests/ui/*` grid tests use, into each blocked state in turn: fetch running (Loading), fetch parked for the backend (AwaitingBackend), no catalog and no fetch (NotLoaded), then a failed fetch (Unavailable). In each state it asserts:
  - the cue's text item is drawn above the table, and its tooltip opens on hover;
  - `Retry` is present for Unavailable and for NotLoaded with a stored catalog request, absent for Loading, AwaitingBackend and a NotLoaded pane with no stored request, and clicking it starts the pane's catalog fetch;
  - at a narrow pane width, `Retry`'s rect lies inside the window's content region;
  - the captured log gains exactly one `Grid pane='<id>' cells are read-only for a catalog reason:` line per state change.
  Once the test lands, drop the manual items from the test plan of the next change to the cue.
  Status: open
  Last-reviewed: 2026-11-23
