# Smatchet glossary

> Living vocabulary. Phase-anchored — entries are appended as new concepts ship.
> Each entry is a one-paragraph definition; rationale links live in `docs/adr/` or
> the originating `docs/design/<slug>.md` plan doc.

## Agentic flow (triage + handoff)

- **AgentProposal** — record-of-an-LLM-suggestion-awaiting-human-approval. Stored in SQLite (`agent_proposals` table). One per LLM-emitted item. Lifecycle: `Pending → Approved | Rejected → Applied | Failed`.
- **ImplementIssue** — the single `AgentProposal.proposedAction` enum value that consents to a coding-harness handoff. All other actions stay triage-only.
- **Triage half** — `agentic-triage-flow` phases T0–T9. Produces proposals.
- **Handoff half** — `agentic-coding-handoff` phases H0–H10. Consumes `ImplementIssue` proposals, spawns Claude Code, drives PR to merge.
- **Coding harness runner** — implementer of `ICodingHarnessRunner`. Phase-1 concrete is `ClaudeCodeLocalRunner`; cloud / Codex / Aider runners are deferred.
- **Sentinel files** — single-writer single-reader JSON files in the harness's worktree (`SEED.json`, `CLARIFICATION_NEEDED.json`, `USER_RESPONSE.json`, `RUN_RESULT.json`, `ERROR.json`, `PR_URL.txt`). Vocabulary defined in `AGENTS.md § Handoff envelope`.
- **Handoff envelope** — the contract between the Smatchet-side `ClaudeCodeLocalRunner` and the spawned-harness-side first delegate (`handoff-implementer`). Documented in `AGENTS.md § Handoff envelope`.
- **HarnessRunState** — FSM tracking a single handoff lifecycle. States: `Pending → Spawning → Running → AwaitingUser ↔ Running → PrOpen ↔ Iterating → Complete | Failed | Cancelled`.
- **PR iteration budget** — `pr_iteration_budget = 10`; cap on how many times `PrCommentWatcher` re-spawns the harness in response to PR comments before forcing user attention.
