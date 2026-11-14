- 2026-11-13 · shared-dropdown-recents review · [debt] · P3 — the project picker copies and rebuilds its whole row list on every frame its popup is open

Details:
`SmatchetProjectPicker::Draw` (`Source/Core/src/Ui/SmatchetProjectPicker.cpp`) calls
`DrawProjectListStatus` and `BuildProjectRows` each frame while the combo is open: a copy of the fetched
project list, a key and a label string per project, and a scan of that list for each saved-catalog row
(at most 16). Before the shared dropdown the copy ran only while the "All projects" section was
expanded; now the list is always shown, so it runs for the whole time the popup is open. It is
invisible on a tracker with tens of projects and an allocation load per frame on a site with thousands.

Concrete next action:
- Keep the built rows in the picker `State`, keyed on the fetch generation and the saved-catalog
  generation, and rebuild only when either changes. Measure the open-popup frame with a 5,000-project
  fixture before and after (`perf-measure` skill).

Status: open
Last-reviewed: 2026-11-13
