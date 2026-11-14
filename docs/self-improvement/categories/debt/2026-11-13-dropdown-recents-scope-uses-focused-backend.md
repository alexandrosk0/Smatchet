- 2026-11-13 · shared-dropdown-recents review · [debt] · P3 — grid cell pickers key their recent picks by the focused pane's tracker, not the pane being drawn

Details:
The single-select, multi-select and labels cell editors build the recents scope with
`app.FocusedCacheBackendKey()` (`Source/Core/src/TicketFieldEditor.cpp`, the two
`smatchet::recents::MakeScopeKey` calls; `Source/Core/src/Tracker/TrackerLabelsEditor.cpp`, one call).
The grid's own context for a pane is `PaneCacheBackendKey(pane.id)`. With two panes on different tracker
instances, a pick made in a cell of the pane that does not hold focus is recorded under the other
instance's scope. Option ids are often small numbers ("3", "10000") that exist on both instances, so
the Recent section there can list a value nobody picked on that instance. Same class as
`2026-10-04-pane-display-value-resolution-uses-focused-backend.md`.

The project picker (`Source/Core/src/Ui/SmatchetProjectPicker.cpp`) has the same split: its project
list and its recents scope follow the focused tracker, while its saved-catalog rows are matched on the
connection of the form that hosts it (`ResolveBackendKindAndEndpoint(cfg, …)`). With a form opened from
one tracker and focus on a pane of another, the popup lists one tracker's projects plus the other's
saved-catalog rows. The Recent seed only takes keys the focused tracker's list holds, so every seeded
row is a valid pick there; a key that exists on both trackers (say "PROJ") can still be seeded on the
strength of its use on the other one.

Concrete next action:
- Carry the drawn pane's cache backend key into `TicketFieldEditor::DispatchEditorByPlan` (it already
  receives the ticket and the field) and into `TrackerLabelsEditor`'s draw call, and build the scope
  from it. Add a doctest on the scope a two-pane fixture produces, or a bucket-E case with two panes.
- Give `SmatchetProjectPicker::Draw` one tracker identity (the hosting form's) for the list fetch, the
  saved-catalog rows and the recents scope.

Status: open
Last-reviewed: 2026-11-13
