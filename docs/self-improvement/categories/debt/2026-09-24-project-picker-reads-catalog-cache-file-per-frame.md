- 2026-09-24 · offline-first sweep · [debt] · P2 — the project picker reads and parses the catalog-cache index every frame while its combo is open

  Details: `Ui/SmatchetProjectPicker.cpp` `DrawRecentSection` calls
  `FieldCatalogCache::ListCachedProjects()` on every frame the picker combo is open. That call reads
  the field-catalog cache from disk and parses its JSON, so an open picker does synchronous file I/O
  plus a parse on the render path (Quality Pillar 2). The cost grows with the number of cached
  projects. Found by the offline-first sweep (docs/plans/active/offline-first.md).

  Concrete next action: load the list once when the popup opens, on a worker, and keep it in the
  picker's state until the popup closes. Show the previous list, or an empty list, while that load
  runs.
  Status: open
  Last-reviewed: 2026-09-24
