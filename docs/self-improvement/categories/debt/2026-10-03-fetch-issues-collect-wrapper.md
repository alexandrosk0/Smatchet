- 2026-10-03 · deviation renewal (markers expiring 2026-12-31) · [debt] · P3 — Jira and Plane collect streamed batches with the same three lambdas

Details:
The non-streaming entry points in `JiraIssueSearch.cpp` and `PlaneIssueSearch.cpp` (`FetchIssues` and the
change probe behind `FetchIssuesChangedSince`) all turn the streamed fetch back into one vector the same
way. Each declares a `results` vector, an `onBatch` lambda that move-appends each batch into it, and a
`shouldCancel` lambda that never cancels. It then calls the backend's streamed fetch and fills the same
out-params from the summary. `dup_audit.py` reports these as clones, exempted at
`JiraIssueSearch.cpp` (`FetchIssues`, the change probe) and `PlaneIssueSearch.cpp` (the change probe).

Concrete next action:
Add one `Tracker/` helper that runs a streamed fetch to completion and returns the tickets with its
summary. Take the fetch as a callable, so each backend still passes its own streamed implementation.
Move the three call sites onto it with their HTTP-fixture tests, then delete the three exemptions
(`revisit=2027-07-31`).

Status: open
Last-reviewed: 2026-10-03
