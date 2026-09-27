# Sync subsystem — agent rules

Scoped rules for `Source/Core/src/Sync/` (ticket sync, offline-queue replay, conflict/merge resolution). Global rules stay in the root [`AGENTS.md`](../../../../AGENTS.md).

This is a **strict lint zone** (root `AGENTS.md` § Tiered enforcement zones).

## Invariants

- **Every backend write goes through the offline queue.** Creates and field edits enqueue via `OfflineQueueService` (`PendingCreate` / `PendingFieldEditRecord`), and comments, worklogs and watches via `PendingActionQueueService` (`PendingActionRecord`), so a change made offline replays on reconnect. A write path that calls the backend directly — skipping the queue — drops the user's edit when they're offline. Flag it.
- **Queue first when offline.** When the connectivity probe says the tracker is unreachable (`IsOfflineState`), a write persists to the queue immediately (`RouteWrite` → `QueueImmediately`, e.g. `FieldEditPipelineService::CommitOrQueue`) instead of first spending the HTTP retry window; online, a retryable failure still falls back to the queue. Queueing never fetches editmeta, and the SQLite enqueue runs on the worker, never the UI thread.
- **A replayed action is sent at most once.** `PendingActionQueueService` persists `sending` before each request; a row found `sending` on open, or whose send failed after the request went out, is `ambiguous` and is checked against the tracker before it is resent (`PendingActionPolicyPure.h`). An action that cannot be checked (a worklog) goes to `needs_review` for the user — never a blind resend.
- **Replay reuses the live pipelines.** Draining a `PendingCreate` reconstructs the `IssueDraft` (`IssueDraftHelpers::FromJson`) and runs it through the same `IssueCreatePipeline::Run` as a live create; a `PendingFieldEditRecord` replays through the same `UpdateField`. Don't fork a separate replay code path — divergence is how offline and online behaviour drift apart.
- **Writes emit an audit pair.** Replayed and live writes both append a `BackendAuditTrail` begin/result attributed via `FieldEditAuditSource`. Conflict detection (3-way merge for rich text) records its decision too.
- **Parent top-up is a worker-side, single-level, non-fatal step.** After the streamed fetch completes cleanly, `TicketSyncService::FetchMissingParentsIntoQueue` collects the parent keys the streamed tickets reference but the result set lacks, fetches them once via `ITrackerIssueReader::FetchIssuesForKeys` (no per-backend branching), and queues them as one extra batch so they land in the cache + active roster like any other ticket. It is skipped when the active view has `HideParents` set or `TrackerConfig::LoadParentIssues` is off (Preferences → Editing → Grid behaviour, default on), and a failed keyed fetch becomes `TrackerIssueFetchSummary::Warning` — never a `FetchError` — so the primary results still apply. Grandparents are not chased (one level, FS parity).
- **Jira extra-host cache keys.** The focused pane's `tickets_v2` namespace is `NormalizeViewsBackendKey` for non-Jira trackers. For Jira it stays `"Jira"` while the live origin is the first instance; an extra Jira host uses `"Jira:<normalized-host>"` so two sites cannot clobber `PROJ-123`. `TicketSyncService::SwapBackendIfTrackerChanged` recreates the `JiraClient` when that host changes (same kind, different origin).

## Before you edit

- The queue payloads (`PendingCreate`, `PendingFieldEditRecord`) live in Persistence (`CachedTicketTypes.h`); the create/update pipelines live in Tracker. Sync is the seam that ties them — a change here usually touches both neighbours.
