# Audit-trail `actor` column

# Status

Accepted (2026-05-21)

# Context

`BackendAuditTrail::AppendBegin` / `AppendResult` writes one row per backend mutation into the SQLite `agent_audit_trail` table. The pre-refactor row carries three discriminators:

- `source` — the backend client that made the call (`"github_client"`, `"jira_client"`, `"plane_client"`).
- `action` — the verb (`"CommentAdd"`, `"LabelAdd"`, `"issue_transition"`, `"issue_update_fields"`).
- `IssueKey` + `OperationId` + timestamp.

Pre-`github-tracker-backend` triage flow, agentic triage actions used the GitHub-specific concrete methods (`GitHubClient::CommentAdd` etc.), while user-driven grid edits routed through a different code path (`AppController::AddIssueCommentPlain`-shaped UI lambdas → eventually the same client). The two call paths happened to land in different actions, so a user could correlate "triage-driven action" by `source=github_client AND action ∈ {triage-call-set}`.

The triage-tracker-agnostic refactor (this plan) reroutes triage writes through `ITrackerClient::UpdateField` / `AddIssueCommentPlain` — the **same virtuals** the grid uses. Audit rows for a triage-driven comment and a user-driven grid comment on a Jira issue post-refactor share `source` (`"jira_client"`), `action` (`"issue_add_comment"`), and shape. Distinguishability collapses.

Two options to recover it:

- (a) Don't recover — accept that audit-trail no longer distinguishes triggers; consumers correlate via timestamp + `agent_proposals` join.
- (b) Add an `actor` column carrying the trigger identity (`user` / `triage` / `ci_react` / `coderabbit_react` / `lua` / `mcp`). Triage write lambdas pass `"triage"`; user grid edits pass `"user"`; spawned-harness paths pass `"ci_react"` / `"coderabbit_react"`; Lua + MCP entry points pass their respective values.

# Decision

**(b)**. Add `actor` column. SQLite migration via the existing `AgentProposalStore`-adjacent migration mechanism. `BackendAuditTrail::AppendBegin` / `AppendResult` signatures gain an `actor` parameter (default `"user"` so the migration is non-breaking — existing call sites that don't pass it default to user-triggered, which is the safe assumption for the grid + UI surfaces).

Triage-flow + ci-react + coderabbit-react + lua + mcp call sites explicitly pass their actor; user-facing UI surfaces accept the default.

# Consequences

- **Schema migration on `agent_audit_trail`** — new column with `DEFAULT 'user'` so existing rows backfill cleanly. Schema-version bump bundled with the triage-refactor PR.
- **Signature change on `BackendAuditTrail::AppendBegin` / `AppendResult`** — additive default parameter; all existing call sites compile unchanged. Bucket-A test verifies the default-actor row shape.
- **Discriminator stability across future code-host integrations** — if Bitbucket / GitLab join the code-host axis later, `actor` (the trigger) and `source` (the backend client) stay orthogonal; new code-host clients add new `source` values without touching `actor`.
- **Audit-consumer simplification** — "what did triage do today" reduces to `SELECT … WHERE actor = 'triage'` instead of a path-derived heuristic. The pre-refactor heuristic was implicit and brittle; this makes the discriminator explicit.
- **Forward-compatibility with Pillar 3 incident triage** — when a Pillar 3 crash investigation needs to know "did a Lua script or the user trigger this last write before the crash", `actor` is the load-bearing column.

Cross-link: [`docs/design/github-tracker-backend.md`](../design/github-tracker-backend.md) § Approach § A.4 + § Risks § Triage regression on GitHub; [`docs/CONTEXT.md`](../CONTEXT.md) § Agentic flow § Audit-trail actor.
