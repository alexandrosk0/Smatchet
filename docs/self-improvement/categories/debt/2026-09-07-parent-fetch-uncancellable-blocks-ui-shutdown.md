- 2026-09-07 · code-review · [debt] · P2 — parent-issue keyed fetch has no cancel token, can block the UI thread on shutdown / backend switch

  Details: `TicketSyncService::FetchMissingParentsIntoQueue` (`Source/Core/src/Sync/TicketSyncService.cpp`)
  calls `ITrackerIssueReader::FetchIssuesForKeys`, which takes no `shouldCancel` predicate — unlike
  `FetchIssuesStreamed`, which polls one per page and is what bounds every `WorkerThread.join()` call the
  UI thread makes (shutdown, backend switch, view switch). The `!shouldCancel()` guard before the keyed
  fetch is checked only on entry; a cancel arriving after that point is never observed. GitHub's
  implementation (`GitHubIssueSearch.cpp`) issues one sequential HTTP request per missing key with no cap;
  Jira chunks at 40 keys/request plus optional per-issue comment paging. A shutdown or backend switch
  during a large parent top-up (many distinct missing parents) blocks the UI thread for the whole fetch —
  Pillar 2 (UI never freezes without a visible cue).
  Found during adversarial review of docs/plans/shipped/parent-issue-hierarchy.md (PR #2198).

  Concrete next action: add a `shouldCancel` (or equivalent) parameter to `ITrackerIssueReader::FetchIssuesForKeys`
  and thread it through the Jira/GitHub/Plane/Linear implementations the same way `FetchIssuesStreamed`
  already does, checking it between request chunks. Enumerator to walk: every `FetchIssuesForKeys` override
  under `Source/Core/src/Tracker/*IssueSearch.cpp` implementing `ITrackerIssueReader`.
  Status: open
  Last-reviewed: 2026-09-07
