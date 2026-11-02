- 2026-09-07 · code-review · [debt] · P2 — missing-parent keyed fetch is uncapped, and one bad key drops the whole batch

  Details: `TicketSyncService::FetchMissingParentsIntoQueue` (`Source/Core/src/Sync/TicketSyncService.cpp`)
  has no ceiling on `missing.size()` — a 5,000-row view with 800 distinct missing parents becomes 800
  sequential GitHub round-trips, or ~20 Jira chunked requests plus per-issue comment paging, every sync.
  Separately, both `JiraIssueSearch::FetchIssuesForKeys` and `GitHubIssueSearch::FetchIssuesForKeys` are
  all-or-nothing (`return FetchResult::Err(...)` on the first non-200), so one deleted or permission-denied
  parent key drops the entire batch — including parents that would have fetched fine. Because parents only
  enter `workerKeepIds` on the fetch's success path, a failed/skipped top-up also lets `SeedStaleDeletionForSession`
  delete previously-cached parents on the next full sync (`FilterStaleIdsRetainedElsewhere` only protects
  rows held by other contexts, not this one).
  Found during adversarial review of docs/plans/shipped/parent-issue-hierarchy.md (PR #2198).

  Concrete next action: (1) cap `missing.size()` with a `summary.Warning` when truncating, same pattern as
  GitHub's existing page-cap warning (`GitHubIssueSearch.cpp` `AppendOutWarning`); (2) seed `workerKeepIds`
  with the referenced parent keys regardless of fetch outcome so a failed/skipped top-up cannot cause the
  stale-purge to delete already-cached parents; (3) consider a per-key retry/skip in `FetchIssuesForKeys`
  instead of all-or-nothing, tracked separately per backend if it's a bigger change.
  Status: fixed (verified 2026-11-01). (2) A walk that fails or hits its hop cap now clears `FullSyncCompleted` (`MarkHierarchyWalkIncomplete`, `TicketSyncService.cpp`), so parents cached by an earlier complete walk are kept, not stale-purged. (3) Keyed fetches go through `FetchKeysIsolatingRefusals` (`Sync/RefusalIsolatingFetchPure.h`), which splits a refused batch to name the bad keys and keep the rest. (1) The walk is bounded by `kMaxParentFetchHops` (16); parents reach only Jira (the one backend whose rows carry `parent`), which chunks keys at 40 per request with a cancel check between chunks.
  Last-reviewed: 2026-09-07
