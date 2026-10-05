- 2026-09-24 · offline-first sweep · [debt] · P2 — the offline-queue counts run a SQLite SELECT on the UI thread every frame

  Details: `Ui/SmatchetUI.cpp` sets `d.cachedPendingFieldEditCount` from
  `app.GetPendingFieldEdits().size()` once per frame. `OfflineQueueService::GetPendingFieldEdits`
  loads every row through `LocalCacheManager::LoadPendingFieldEdits`, only to count them. The status
  bar (`Ui/SmatchetStatusBarUi.cpp`) does the same for creates: `app.GetPendingCreateCount()` loads
  every pending create through `LoadPendingCreates()` and returns `.size()`. Both reads are synchronous
  SQLite I/O on the render path (Quality Pillar 2). They are cheap while the queue is empty, but they
  grow with the queue, which is exactly the offline case the queue exists for. Found by the
  offline-first sweep (docs/plans/active/offline-first.md).

  Concrete next action: keep both counts in `OfflineQueueService` as atomics. Update them on the worker
  after every enqueue, replay, archive and restore (the same places that already touch the tables).
  The UI then reads the atomics and never queries SQLite in a frame.
  Re-scoped 2026-10-04 (backlog-sweep-2026-10): the create count is fixed (backlog-sweep PR #2296: GetPendingCreateCount() and a new GetPendingFieldEditCount() are atomic loads, recounted on a worker after enqueue/delete/restore/sweep, at the end of each replay pass, on a new cache object and on a replay-tick mismatch; the offline.replay_now dry-run uses the exact count). Remaining: Ui/SmatchetUI.cpp still sets d.cachedPendingFieldEditCount from app.GetPendingFieldEdits().size() every frame — routing it to the new atomic needs an AppController::GetPendingFieldEditCount() delegator in AppController.h, which was in another session's plan-lock.
  Status: open
  Last-reviewed: 2026-10-04
