- 2026-05-12 · offline-sync · [debt] · P3 · DEFERRED — `SaveFieldCatalogSnapshot` accumulated 4 extra primitive args; a `FieldCatalogSaveContext` struct would prevent future drift
  Details: callers already had each arg in scope; bundling them into one struct keeps the call site narrow as more per-axis state lands.
  Concrete next action: small C++ refactor — bundle into the next PR that touches `SaveFieldCatalogSnapshot`. Don't open a standalone refactor PR; the win shows up only when adding the next per-axis arg, which is when the bundling decision gets reviewed in context.
  Re-filed 2026-10-04 (backlog-sweep-2026-10): moved from process.md — this is product tech-debt, not workflow friction.
  Status: deferred
  Last-reviewed: 2026-10-04
