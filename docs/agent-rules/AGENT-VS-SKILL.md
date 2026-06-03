# Agent vs skill — the decision rubric

> The live home of the skill-vs-agent criteria. Lifted verbatim from
> [`docs/plans/shipped/agents-skill-conversion.md`](../plans/shipped/agents-skill-conversion.md)
> § Decision criteria (which named this file as its intended home but left it
> uncreated). Use this when deciding whether a new recurring procedure should be
> an **agent** (`agents/core/` or `agents/project/`) or a **skill**
> (`agents/_shared/skills/<name>/SKILL.md`).

## Skill when **ALL** hold

- Deterministic procedure or rule-sheet (no investigation loop).
- Bounded context footprint (≤ 3 file reads, ≤ 2 file edits typical).
- No multi-round user-feedback or PR-comment loop.
- No parallel-dispatchable subtask spawn.
- Output is direct action / inline result, not a return-summary the main thread synthesizes.

## Agent when **ANY** hold

- Heavy exploration / context-bloat risk.
- Multi-round loop.
- Isolated worktree / spawned-child orchestration.
- Parallel-dispatchable.
- Delegates to other agents.

## Cross-harness consideration

Codex + Cursor read `agents/*.md` directly per the [agents.md spec](https://agents.md/) — they have no skill concept today. Until the spec (or a per-harness adapter) covers skills, conversions that **delete** an agent file break cross-harness discovery. **Dual-publish** is the safe pattern: keep the agent `.md` for Codex/Cursor, add a Claude `@`-import `SKILL.md` alias. A pure conversion (delete the agent, keep only the skill) is safe **only** under an explicit Claude-only gate — not the default.

A **skill-only** helper (no agent twin) is legitimate when the procedure is orchestrator-inline + bounded + needs no cross-harness agent discovery — register it in `SKILL_ONLY_HELPERS` in [`agents/scripts/core/test-skill-vs-agent-parity.sh`](../../agents/scripts/core/test-skill-vs-agent-parity.sh) so the parity guard doesn't demand a matching `agents/` file (precedents: `drain-memory`, `author-plan-doc`).
