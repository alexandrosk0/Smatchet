# Audit-trail `actor` field

# Status

**Withdrawn (2026-05-21)** — the triage tracker-agnostic refactor that drove this ADR was stripped from the [`github-tracker-backend`](../design/archive/github-tracker-backend.md) plan on the same day. Without the refactor, triage and grid writes continue to land on distinct call paths (concrete `GitHubClient::*` methods vs `ITrackerClient::UpdateField` virtuals), so the `source`+`action`+timestamp heuristic continues to discriminate triggers as it does today. This ADR remains in-tree as a reference: when the deferred refactor lands, re-propose under a new ADR number with status Accepted; reuse the design here verbatim.

Original status: Accepted (2026-05-21) — revoked same day.

# Context

`BackendAuditTrail::AppendBegin` / `AppendResult` writes one JSON-Lines row per backend mutation into `smatchet_backend_audit.jsonl` (async via `BackendAuditTrail::AuditWriter` at `Source_Core/src/BackendAuditTrail.cpp:152, 286-368`). **There is no SQLite audit table** — `AgentProposalStore` owns the SQLite schema for `agent_proposals` + `agent_poll_cursor` but does NOT carry an `agent_audit_trail` table. The pre-refactor row carries three discriminators:

- `source` — the backend client that made the call (`"github_client"`, `"jira_client"`, `"plane_client"`).
- `action` — the verb (`"CommentAdd"`, `"LabelAdd"`, `"issue_transition"`, `"issue_update_fields"`).
- `IssueKey` + `OperationId` + timestamp.

Pre-`github-tracker-backend` triage flow, agentic triage actions used the GitHub-specific concrete methods (`GitHubClient::CommentAdd` etc.), while user-driven grid edits routed through a different code path (`AppController::AddIssueCommentPlain`-shaped UI lambdas → eventually the same client). The two call paths happened to land in different actions, so a user could correlate "triage-driven action" by `source=github_client AND action ∈ {triage-call-set}`.

The triage-tracker-agnostic refactor (this plan) reroutes triage writes through `ITrackerClient::UpdateField` / `AddIssueCommentPlain` — the **same virtuals** the grid uses. Audit rows for a triage-driven comment and a user-driven grid comment on a Jira issue post-refactor share `source` (`"jira_client"`), `action` (`"issue_add_comment"`), and shape. Distinguishability collapses.

Two options to recover it:

- (a) Don't recover — accept that audit-trail no longer distinguishes triggers; consumers correlate via timestamp + `agent_proposals` join.
- (b) Add an `actor` column carrying the trigger identity (`user` / `triage` / `ci_react` / `coderabbit_react` / `lua` / `mcp`). Triage write lambdas pass `"triage"`; user grid edits pass `"user"`; spawned-harness paths pass `"ci_react"` / `"coderabbit_react"`; Lua + MCP entry points pass their respective values.

# Decision

**(b)**. Add `actor` field. JSONL row shape gains an `actor` key on every new line; reader tolerates legacy lines (pre-refactor file lines without the key) and treats absent key as `"user"`. `BackendAuditTrail::AppendBegin` / `AppendResult` / `AppendEvent` signatures gain a defaulted `actor` parameter (default `"user"` — safe for grid + UI surfaces that don't pass it). `AuditEvent` struct (`Source_Core/include/BackendAuditTrail.h`) gains `std::string Actor = "user";` member; struct-builder call sites that omit the field default-init it.

Triage-flow + ci-react + coderabbit-react + lua + mcp call sites explicitly pass their actor; user-facing UI surfaces accept the default. `AppController` handoff AuditSink lambda (line 1873-1882) must explicitly stamp `ev.Actor` from the controller's dispatch_source context before invoking the sink — without this, every handoff-triggered backend write logs `actor="user"`.

# Consequences

- **No SQLite migration** — the JSONL substrate is append-only. New lines carry the key unconditionally; old lines stay readable. Reader uses `value("actor", "user")`-style tolerant deserialisation.
- **Signature change on `BackendAuditTrail::AppendBegin` / `AppendResult` / `AppendEvent`** — additive default parameter at the end of the parameter list; all existing free-function call sites compile unchanged. Bucket-A test verifies the default-actor row shape AND that legacy-line read tolerates absent key.
- **`AuditEvent` struct field added** — every struct-builder call site (notably `AppController.cpp` handoff AuditSink lambda) needs explicit `ev.Actor = …` plumbing or accepts the default. Default-init covers correctness; explicit-init covers discriminator accuracy.
- **Discriminator stability across future code-host integrations** — if Bitbucket / GitLab join the code-host axis later, `actor` (the trigger) and `source` (the backend client) stay orthogonal; new code-host clients add new `source` values without touching `actor`.
- **Audit-consumer simplification** — "what did triage do today" reduces to JSONL-grep `actor=triage` instead of a path-derived heuristic. The pre-refactor heuristic was implicit and brittle; this makes the discriminator explicit.
- **Forward-compatibility with Pillar 3 incident triage** — when a Pillar 3 crash investigation needs to know "did a Lua script or the user trigger this last write before the crash", `actor` is the load-bearing column.

Cross-link: [`docs/design/archive/github-tracker-backend.md`](../design/archive/github-tracker-backend.md) § Out of scope § Agentic triage tracker-agnostic refactor (the deferred work this ADR was originally drafted for).
