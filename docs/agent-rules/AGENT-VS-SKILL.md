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

A **skill-only** helper (no agent twin) is legitimate when the procedure is orchestrator-inline + bounded + needs no cross-harness agent discovery — register it in `SKILL_ONLY_HELPERS` in [`agents/scripts/core/test-skill-vs-agent-parity.sh`](../../agents/scripts/core/test-skill-vs-agent-parity.sh) so the parity guard doesn't demand a matching `agents/` file (precedents: `drain-memory`, `author-plan-doc`, `gate-escape-postmortem`).

## Extracting procedure-bodies out of a heavy agent (not just whole-agent conversion)

The rubric above decides where a *new* procedure lives. It applies equally **inside** an existing heavy agent: when an agent prompt grows large, the bloat is usually **deterministic procedure + verbatim shell/code recipe** inlined next to genuine reasoning. The recipe is a skill living in the wrong place. Extract it, leaving the agent a thin reasoning/routing layer (the goal is a **readable** agent — the size gate `agent_size_audit.py` keeps it that way; see AGENTS.md § Project rules § Prompt/contract size).

**The reasoning/recipe seam** — cut strictly along this line:

- **STAYS in the agent** (the *judgment* — what you read to understand how it thinks): scope/boundary, hypothesis ranking, evidence/metric choice, multi-round loops, refusal/hard rules, hand-off, report shape. Anything the agent's logic depends on inline stays inline (never split mid-reasoning). A **hot-path** rule the agent always applies stays inline too — relocating an always-needed rule *hurts* readability (now you read two files for the always-case). Only **conditional / standalone-recipe** sections (the long shell blocks the agent runs occasionally) extract cleanly.
- **EXTRACTS to a skill** (the *mechanics* — verbatim shell you copy-paste-run, rarely re-read for logic): instrumentation recipes, the exact command sets, cleanup commands.

**Cross-harness mechanism (locked): summary + explicit pointer, never auto-injection.** The long verbatim blocks move to `SKILL.md`; the agent retains a compact *what + when + "full recipe: `agents/_shared/skills/<name>/SKILL.md`"* pointer. Claude Code loads the skill on demand (context saved); Codex/Cursor — which read `agents/*.md` literally and have no skill concept — see the summary + a readable path (graceful degradation: they execute from the summary or open the file). Each such extraction is a **skill-only** helper → register it in `SKILL_ONLY_HELPERS`. **Rejected:** `@`-import of shared snippets (Codex/Cursor don't expand `@`, would lose the content); dual-publishing the full recipe (doesn't shrink the agent at all — defeats the goal). The skill-only registration mechanism's precedent is `gate-escape-postmortem`; `perf-instrument` is the precedent for the *content/shape* of an instrumentation-recipe skill (but it is dual-published, with an agent twin — do not copy its twin wiring).
