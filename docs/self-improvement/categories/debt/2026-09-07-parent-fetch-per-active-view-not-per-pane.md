- 2026-09-07 · code-review · [debt] · P2 — worker-side missing-parent fetch gates on the ACTIVE view; the grid projection gates per pane

  Details: `TicketSyncService::FetchMissingParentsIntoQueue` decides whether to skip the keyed parent fetch
  by reading `ConfigManager::FindActiveViewOrFirst(viewsCopy.Views, viewsCopy.ActiveViewId)`'s `HideParents`
  and the global `TrackerConfig::LoadParentIssues` — one decision for the whole sync. But the grid's row
  projection (`SmatchetActiveProjectGridTable.cpp`, `HierarchyOptionsForView(activeViewForGrid)`) reads
  the hierarchy options **per pane** (ADR-0018 multi-pane views). A background pane whose own view wants
  Story group / does not hide parents gets no parent top-up whenever the globally-active view happens to
  hide parents — the two switches read different scopes for the same feature.
  Found during adversarial review of docs/plans/shipped/parent-issue-hierarchy.md (PR #2198).

  Concrete next action: either (a) make the worker-side gate check every open pane's view (fetch parents
  if ANY pane wants them), or (b) document the active-view-only scope as an intentional simplification in
  docs/guides/parent-hierarchy.md and drop the cost only when literally no pane could use the result.
  Enumerator: `g_ui.gridPanes` (or the pane-view resolver used by `HierarchyOptionsForView` callers) is the
  per-pane view source of truth to reconcile against.
  Status: open
  Last-reviewed: 2026-09-07
