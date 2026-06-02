# Sync subsystem — agent rules

Scoped rules for `Source/Core/src/Sync/` (ticket sync, offline-queue replay, conflict/merge resolution). Global rules stay in the root [`AGENTS.md`](../../../../AGENTS.md).

This is a **strict lint zone** (root `AGENTS.md` § Tiered enforcement zones).

## Invariants

- **Every backend write goes through the offline queue.** Creates and field edits enqueue via `OfflineQueueService` (`PendingCreate` / `PendingFieldEditRecord`) so an edit made offline replays on reconnect. A write path that calls the backend directly — skipping the queue — drops the user's edit when they're offline. Flag it.
- **Replay reuses the live pipelines.** Draining a `PendingCreate` reconstructs the `IssueDraft` (`IssueDraftHelpers::FromJson`) and runs it through the same `IssueCreatePipeline::Run` as a live create; a `PendingFieldEditRecord` replays through the same `UpdateField`. Don't fork a separate replay code path — divergence is how offline and online behaviour drift apart.
- **Writes emit an audit pair.** Replayed and live writes both append a `BackendAuditTrail` begin/result attributed via `FieldEditAuditSource`. Conflict detection (3-way merge for rich text) records its decision too.

## Before you edit

- The queue payloads (`PendingCreate`, `PendingFieldEditRecord`) live in Persistence (`CachedTicketTypes.h`); the create/update pipelines live in Tracker. Sync is the seam that ties them — a change here usually touches both neighbours.
