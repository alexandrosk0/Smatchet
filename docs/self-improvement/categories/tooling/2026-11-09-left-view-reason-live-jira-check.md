# The left-view reason lookup has never run against a real Jira

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-11-09
- **Observed on**: the left-view history grid (`docs/plans/active/left-view-history-grid.md`, § Verification "Manual residue").
- **Status**: open

## What happened

`JiraClient::FetchIssueFieldChanges` (`search/jql` with `expand=changelog`, batches of 40) is only exercised by the fixture-driven fake Jira. The changelog parser (`AllChangesFromIssueJson`) has doctests, but the HTTP request, the paging and the refusal handling have never met a live site. So nothing checks that a real status transition yields the expected "Status: A -> B" reason with its author, or that a batch naming a hidden issue is refused the way the refusal isolation assumes.

## Concrete next action

Add a credentialed, opt-in lane (local script first, CI later behind a secret) against a sandbox Jira project: create an issue matching a view's JQL, list the view, transition the issue out, reload, and assert the recorded row's reason kind, text and author through `left_view.list`. Never point it at a production site.
