- 2026-09-07 · code-review · [debt] · P3 — `issuelinks` is now requested on every Jira search page for every user, to serve a fallback most instances never hit

  Details: `BuildFetchFieldListsFromView` (`Source/Core/src/Tracker/JiraIssueMappingPure.cpp`) unconditionally
  adds `"issuelinks"` to the Jira search `fields` parameter so `ApplyIssueLinksParentFallback` can find an
  inward "is part of" link when the native `parent` field is absent (classic-project issue types). The
  payload cost of that extra field lands on every Jira sync for every user, including the majority whose
  instance always populates `parent` and never exercises the fallback.
  Found during adversarial review of docs/plans/shipped/parent-issue-hierarchy.md (PR #2198).

  Concrete next action: gate `issuelinks` inclusion on `TrackerConfig::LoadParentIssues` (off already means
  the user doesn't want parent hierarchy data at all) or measure the actual payload delta first and close
  this as observational if it's negligible. Low priority — flagged for awareness, not urgent.
  Status: open
  Last-reviewed: 2026-09-07
