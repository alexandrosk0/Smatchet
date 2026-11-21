- 2026-11-16 · bulk-edit-rows review · [debt] · P3 — the `ticket.set_field_bulk` worker and a running bulk job cannot be cancelled

Details:
`ticket.set_field_bulk` called on the UI thread (the command palette, a Lua ticket action) runs its rows on a
`LaunchBackgroundTask` worker (`RunBulkRows` in
`Source/Core/src/Commands/Builtin/BuiltinCommands_TicketMutations.cpp`). The worker stops early only when the app
is shutting down (`IsShuttingDown()`); there is no way for the user to cancel a long job, and a grid-started job
has no cancel either: its rows wait in `queuedFieldEdits` until the pump sends them. A mis-aimed bulk edit on many
rows has to run to the end (Revert, planned in P2 of `docs/plans/active/bulk-edit-rows.md`, is the way back).

Why it was not fixed in the bulk-edit finish PR: a cancel needs a visible control (a toast action or a button in
the grid) and a decision about what cancel means for rows already queued offline (keep or delete them). That is a
UX decision for P2, together with Revert, not a defect of the shipped path.

Concrete next action:
- When P2 adds Revert, add a "Stop" action to the in-flight summary toast that sets a per-job cancel flag. The
  grid pump skips and drops the job's remaining `queuedFieldEdits` rows (reported as skipped: "cancelled"), and
  `RunBulkRows` checks the same flag next to `IsShuttingDown()`. Rows already sent or queued offline stay.
- Cover it with a bucket-E case: start a 12-row job against a slowed fake backend, cancel after the first row,
  assert the job ends with the rest skipped.

Status: open
Last-reviewed: 2026-11-16
