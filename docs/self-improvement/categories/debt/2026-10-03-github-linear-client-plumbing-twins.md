- 2026-10-03 · deviation renewal (markers expiring 2026-12-31) · [debt] · P3 — GitHub and Linear clients repeat the same fetch and edit plumbing

Details:
`GitHubClient.cpp` and `LinearClient.cpp` carry several token-identical stretches that `dup_audit.py`
reports as clones. The exemptions sit in `GitHubClient.cpp`:
- the constructor's auth logging;
- the degrade-to-warning tail of the field-catalog fetch;
- the streamed-page emit lambda in `FetchIssuesStreamed` and the summary fill around it;
- the `UpdateField` routing (build the single-field payload, then `UpdateIssueFields`), which Jira repeats too.

They are not interface boilerplate: each is logic that both backends run the same way, and a fix to one
copy has to be made in the other by hand. All of it lives in `Tracker/` beside the shared `TrackerHttpUtils`,
so a shared helper couples no independent subsystems.

Concrete next action:
1. Extract the streamed-page emitter and summary fill into a small `Tracker/` helper used by both
   `FetchIssuesStreamed` implementations.
2. Give `ITrackerIssueMutations::UpdateField` a default body that builds the payload and calls
   `UpdateIssueFields`, and drop the per-client copies that match it.

Cover both with the existing GitHub and Linear HTTP-fixture tests, then delete the five exemptions
(`revisit=2027-06-30`).

Re-scoped 2026-10-06 (backlog-sweep-2026-10): part (2) is done: ITrackerIssueMutations::UpdateField has a default body that routes through UpdateIssueFields, and the GitHub/Linear/Jira/Plane copies are gone (PR #2296). Remaining part (1): extract the streamed-page emitter and summary fill shared by the GitHub and Linear FetchIssuesStreamed, plus the constructor auth-logging and degrade-to-warning twins; four plumbing-twins markers remain in GitHubClient.cpp.
Status: open
Last-reviewed: 2026-10-06
