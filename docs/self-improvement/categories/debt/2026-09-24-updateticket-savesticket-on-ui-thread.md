- 2026-09-24 · offline-first sweep · [debt] · P2 — `AppController::UpdateTicket` writes the ticket to SQLite on the UI thread

  Details: `AppController::UpdateTicket` (`AppController_CatalogAndFieldEdit.cpp`) calls
  `Cache->SaveTicket(capturedKey, ticket)` inline, and its callers apply field-edit results on the
  UI thread (`ApplyFieldEditResult` through the grid pipeline's main-thread post-back). The write runs
  inside `RunWriteTxnWithBusyRetry`, whose busy-retry deadline can hold the frame, so a contended
  cache stalls the UI (Quality Pillar 2). Every queued offline edit also passes through this path when
  it is applied locally. Found by the offline-first sweep (docs/plans/active/offline-first.md).

  Concrete next action: post the `SaveTicket` call to a worker. Keep the key-and-generation latch
  that precedes it (issue #1081), and pass the latched key into the worker so the write still lands
  under the captured backend. The in-memory ticket update and `RefreshLocalData()` stay on the UI
  thread.
  Status: open
  Last-reviewed: 2026-09-24
