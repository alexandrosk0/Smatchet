- 2026-11-16 · bulk-edit-rows review · [debt] · P2 — quitting during a bulk edit drops the rows that were not sent yet

Details:
A bulk edit fans out into one `PendingFieldEdit` per row in `UiDrawSession::queuedFieldEdits`, and
`PumpGridFieldEdits` sends them one at a time (`hasInFlightEdit`). A row reaches the offline queue only when
its own send runs. If the user quits while rows are still waiting, `DrainUiDrawSessionFuturesBeforeAppTeardown`
(`Source/Core/src/Ui/SmatchetUI_Layout.cpp`, the `unsentBulkRows` block) counts them with a `LOG_WARN` and drops
them. The user saw "Saving N rows..." and a half-applied job; the rest of the edit is lost without a toast.
This is a Pillar-6 gap (an edit the user made must not vanish), narrow in practice: a job of N rows takes about
N round-trips, so the window is the length of one job.

Why it was not fixed in the bulk-edit finish PR: the fix is a new queue-only entry on
`FieldEditPipelineService` (build the payload and the ADR-0016 base exactly as `QueuePreparedEdit` does, without a
network attempt) plus a shutdown drain that calls it for each unsent row on a thread that may already be winding
down. No bucket can drive a quit-and-relaunch today
(`2026-08-05-no-bucket-e-shutdown-relaunch-primitive.md`), so the change would ship unverified on the exact path
that loses data. It also needs a decision on whether a quit may spend time on N queue writes.

Concrete next action:
- Add `FieldEditPipelineService::QueueWithoutSending(const FieldEditCommitRequest&)` (bind the target, require
  `FieldEditSupportsOfflineQueue`, call `QueuePreparedEdit`), with a doctest over the fake deps.
- In the shutdown drain, convert each unsent bulk row (`BulkJobId != 0`) with the same request builder the commit
  worker uses and queue it; report how many were saved in the log. Keep the `LOG_WARN` for rows that could not be
  queued.
- Verify with a bucket-E or CLI test once the shutdown/relaunch primitive exists; until then a doctest on the
  queue-only entry is the coverage.

Status: open
Last-reviewed: 2026-11-16
