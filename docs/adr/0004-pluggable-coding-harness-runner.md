# Pluggable coding-harness runner

# Status

Accepted (2026-05-18)

# Context

The handoff half of the agentic flow (see [`docs/design/agentic-flow-implementation.md`](../design/agentic-flow-implementation.md) and the architecture rationale in [`docs/design/agentic-coding-handoff.md`](../design/agentic-coding-handoff.md)) spawns an external coding harness to drive an approved `ImplementIssue` proposal through to a merged PR. Two paths exist:

- (a) Hard-wire the controller to a specific harness (the phase-1 target — Claude Code's local CLI), or
- (b) Abstract the spawn / sentinel / iteration mechanics behind an `ICodingHarnessRunner` interface so future runners (cloud Claude, Codex / OpenAI Agents, Aider, generic) can drop in without controller surgery.

A separate question is what the security boundary actually is. The phase-1 harness is invoked with `--permissions bypassPermissions` so the harness can edit / build / test inside its assigned worktree without prompting; that flag is **not** an OS-level sandbox. The boundary the controller relies on is two-fold: the **environment allow-list** passed to the spawn, and the **current working directory** the harness is bound to. The allow-list is locked in [`docs/design/agentic-flow-implementation.md`](../design/agentic-flow-implementation.md) plan decision #7 as `{PATH, HOME, USER, USERPROFILE, TEMP, TMP, SYSTEMROOT, GH_TOKEN, GITHUB_TOKEN, ANTHROPIC_API_KEY}`. Everything else (user shell aliases, ambient cloud creds, parent-process env leakage) is stripped before `Spawn` returns.

# Decision

Adopt path (b). `ICodingHarnessRunner` is the abstraction; `ClaudeCodeLocalRunner` is the phase-1 concrete. The interface is shaped around the stream-json + sentinel-file model the phase-1 harness uses (see plan decision #7 + `AGENTS.md § Handoff envelope` once it lands in H2); future runners that don't speak that protocol natively will need an adapter shim per-runner, but the controller surface stays stable.

The env allow-list is part of the interface contract, not a `ClaudeCodeLocalRunner` implementation detail: every `Spawn` implementation must enforce the allow-list at the boundary plus cwd containment to the assigned worktree. That contract is the documented security boundary for the handoff half — `bypassPermissions` is trust, not isolation.

# Consequences

- **Future runners drop in without controller refactor.** Cloud Claude, Codex / OpenAI Agents, and Aider can each ship as a separate `ICodingHarnessRunner` impl + adapter shim. The `AgenticHandoffController`, the `PrCommentWatcher`, and the `HarnessRunState` FSM never need to know which runner is active.
- **`bypassPermissions` is a security trust boundary, not a sandbox.** Runners that implement `Spawn` must enforce the env allow-list and cwd containment themselves; the orchestrator does not re-validate after spawn. Reviewers of any new runner impl verify the allow-list is applied at the spawn site, not "somewhere upstream".
- **The `handoff-implementer` and `pr-iterator` agent prompts are runner-agnostic.** Both agent files (landing in H2 / H7 respectively) describe the work the harness must do — read SEED.json, run the slice, write RUN_RESULT.json / PR_URL.txt — not the harness invoking them. A different runner whose internal agent vocabulary differs still drives the same envelope.
- **Trade-off: the interface is shaped around Claude Code's stream-json + sentinel-file model.** Cloud Claude over HTTP, Codex with its own tool-call format, and Aider's REPL-style loop will each need a small adapter shim that translates their native event stream into the sentinel-file vocabulary the controller expects. This is documented as a known cost of choosing (b) over a per-harness controller per-runner; the alternative — controller code paths per harness — was rejected as a larger long-term cost.
- **The interface lives in a future phase, not this ADR.** H2 introduces `CodingHarnessTypes` (shared structs) and the `ICodingHarnessRunner` header itself; this ADR records the architectural decision so that phase's PR has a settled rationale to point at instead of re-litigating the abstraction at implementation time.
