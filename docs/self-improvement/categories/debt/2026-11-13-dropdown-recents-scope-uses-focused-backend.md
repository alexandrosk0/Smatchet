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

The project picker (`Source/Core/src/Ui/SmatchetProjectPicker.cpp`) has a split of its own: its project
list, its recents scope and the backend the create forms submit to are all the focused tracker's
(`LaunchIssueCreate_` latches `focusedContext()`), while its saved-catalog rows are matched on the
callers' `d.cfg` (`ResolveBackendKindAndEndpoint(cfg, …)`), the configured-tracker snapshot of
`ConfigManager::Load()`. That `backendKind`/`endpoint` pair also shapes the live list: it picks the row
key (`detail::AllProjectKey`: the id for Plane, `key` otherwise) and the label (`MakeRowLabel`). When the
focused pane is not the configured tracker, the popup lists the focused tracker's projects plus the
configured tracker's saved-catalog rows, and with a Jira config over a focused Plane pane the live rows
key on an empty `key` and are dropped. The Recent seed only takes keys the
focused tracker's list holds, so every seeded row is a valid pick there; a key that exists on both
trackers (say "PROJ") can still be seeded on the strength of its use on the other one. Scoping the
recents by `d.cfg` instead would be wrong (it breaks the match with the list and the submit target).

Concrete next action:
- Carry the drawn pane's cache backend key into `TicketFieldEditor::DispatchEditorByPlan` (it already
  receives the ticket and the field) and into `TrackerLabelsEditor`'s draw call, and build the scope
  from it. Add a doctest on the scope a two-pane fixture produces, or a bucket-E case with two panes.
- Resolve `SmatchetProjectPicker`'s `backendKind`/`endpoint` from the focused pane's connection (not
  `d.cfg`) for the row keys, labels and saved-catalog rows alike, so the list, the saved-catalog rows and
  the recents scope name the tracker the create submits to.

Status: open
Last-reviewed: 2026-11-13
