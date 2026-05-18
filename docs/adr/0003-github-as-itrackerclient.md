# GitHub as an `ITrackerClient` backend

# Status

Accepted (2026-05-18)

# Context

The agentic triage half (see [`docs/design/agentic-flow-implementation.md`](../design/agentic-flow-implementation.md) and the architecture rationale in [`docs/design/agentic-triage-flow.md`](../design/agentic-triage-flow.md)) polls issues from external trackers (Jira, Plane, GitHub) and feeds them into the proposal pipeline. The agent core only talks to one abstraction today — `ITrackerClient` — and the two existing backends (`JiraClient`, `PlaneClient`) implement it. Two paths exist for adding GitHub:

- (a) add a parallel `IGitHubClient` abstraction tailored to the REST surface (Issues / Comments / PRs / Labels / Assignees / State), or
- (b) implement GitHub as an `ITrackerClient` backend, mapping the GitHub REST surface onto the existing core methods and returning documented "unsupported" sentinels for methods the agent core never calls against GitHub (JQL, sprints, worklog).

# Decision

Implement GitHub as an `ITrackerClient` backend. The agent core only talks `ITrackerClient`; a parallel abstraction would force a second adapter layer at every triage / proposal-pipeline boundary that currently consumes one interface.

# Consequences

- Some `ITrackerClient` methods (JQL, sprints, worklog) return documented "unsupported" sentinels when the backend is GitHub. Callers that care must branch on the sentinel; agentic-flow callers do not call those methods, so the sentinel is invisible to the production triage path.
- Trade-off accepted because:
  1. **The agentic flow doesn't use the unsupported methods.** JQL / sprints / worklog are Jira-centric features the agent core never invokes during triage or handoff. The sentinel cost is paid in tests, not in shipped code paths.
  2. **A future GitHub-native abstraction can absorb the sentinel without breaking other backends.** If a GitHub-specific feature (e.g. PR-review-comment threading, GitHub Actions wiring) needs a richer interface than `ITrackerClient` exposes, that feature can land as an additive sibling interface (`IGitHubExtensions` or similar) without retrofitting the existing backends or the proposal pipeline.
  3. **GitHub's REST surface maps cleanly to `ITrackerClient` core methods that agentic uses.** Issues, Comments, PRs, Labels, Assignees, and State all have one-to-one analogues in the existing interface. The mapping is mechanical, not lossy.
- Pre-existing `JiraClient` / `PlaneClient` semantics stay unchanged — GitHub joins the union, it does not perturb the existing backends.
- The "unsupported sentinel" contract is documented per-method in the eventual `GitHubClient` header (lands in a later phase); reviewers of that PR will verify the sentinel surface against this ADR.
