# Persistence subsystem — agent rules

Scoped rules for `Source/Core/src/Persistence/` (SQLite-backed local cache, audit trail, field-edit audit source). Global rules stay in the root [`AGENTS.md`](../../../../AGENTS.md).

This is a **strict lint zone** (root `AGENTS.md` § Tiered enforcement zones).

## Invariants

- **SQLite schema changes are additive only.** Add columns/tables; never drop, rename, or change the type of an existing column in place. The local cache is a long-lived per-user file — a destructive migration corrupts or discards a user's offline state. Flag any `DROP` / `ALTER ... RENAME` / type change. A genuine schema evolution needs a versioned migration step, not an in-place edit.
- **All SQLite work is off the UI thread.** `SQLite::Database` calls run on a worker thread, never inline in a render frame; the worker posts only the result back via `MainThreadDispatcher::PostToMainThread`, and chunks large writes (`SmatchetChatPersistWorker` is the reference pattern; this is the Pillar-2 UI-thread rule — see `Source/Core/src/Ui/AGENTS.md`).
- **Audit trail is append-only + redacted.** `BackendAuditTrail` entries are begin/result pairs; secrets/tokens go through the redaction helpers before they land. Don't log raw payloads.

## Before you edit

- The cache owns `CachedTicket` (the in-memory + on-disk issue row) and `PendingCreate` / `PendingFieldEditRecord` (offline-queue payloads). Tracker reads/writes these; Sync replays them. A schema touch here ripples to both — check `Source/Core/src/Tracker/` and `Source/Core/src/Sync/`.
