- 2026-09-27 · CodeRabbit on the S9 PR (offline-first) · [debt] · P3 — a lost UI post-back leaves an in-flight latch set

  Details: UI in-flight latches clear only in the callback a worker posts back with
  `IMainThreadPoster::PostToMainThread`. Examples are the worklog Save latch, the comments-modal post
  latch, the Annotate commit latch and the grid field-edit pump. `MainThreadDispatcher::PostToMainThread`
  (`Source/Core/include/MainThreadDispatcher.h`) can lose that callback in two ways:
  - It silently drops the OLDEST queued task once the queue reaches `kMaxQueueSize`.
  - It throws if `push_back` cannot allocate.

  In either case the latch stays set until restart, and that feature is disabled for that ticket. This
  is not a crash, and it needs a saturated dispatcher or an out-of-memory condition.
  `Ui/PendingActionSubmitAsync.h` retries once from its exit guard and logs, which is all a single
  helper can do.

  Concrete next action: give the dispatcher a lossless mode for completion callbacks. For example:
  - never drop a task marked as a completion;
  - reserve queue capacity up front so `push_back` cannot allocate;
  - count and log every dropped task.
  Alternatively, give latches a staleness timeout that a UI-thread tick clears, with a visible cue.
  Status: open
  Last-reviewed: 2026-09-27
