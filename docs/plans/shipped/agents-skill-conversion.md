# Evaluate current agents — skill conversion candidates (v3, conservative-only, dual-publish)
<!-- plan-date: 2026-05-19 -->

## Context

Smatchet ships **24 agents** (`agents/*.md`) and **3 skills** (`grill-with-docs`, `scratchpad-recall`, `token-tracking`). No documented decision framework for choosing agent vs skill — the only explicit choice in the tree is `docs/agent-rules/delegation.md:128` flagging `grill-with-docs` as "skill, not agent" without rationale.

This document was revised twice. v1 listed 5 conservative candidates; v2 reduced to 2 after fact-check; **v3 (this version)** keeps the same 2 candidates but switches from a **destructive conversion** (delete agent file, replace with skill) to a **non-destructive dual-publish** model (keep agent file as canonical content, add a Claude-Code-only SKILL.md alias that imports it) — because v2 broke cross-harness discovery on Codex / Cursor.

v3 changes (all driven by review findings):

- **P1 — cross-harness break (fixed)**: deleting `agents/perf-instrument.md` + `agents/perf-measure.md` breaks Codex (`setup-harness.sh:133-142`: reads `agents/*.md` directly, has no skill concept) and Cursor (same — `.cursor/rules/agents.mdc` points at `AGENTS.md` + `agents/`). v3 keeps both agent files in place. SKILL.md aliases at `agents/_shared/skills/<name>/SKILL.md` use Claude Code's `@`-import to source the canonical agent file. Codex / Cursor users continue invoking as agent unchanged; Claude Code gets a lighter skill path.
- **P1 — `setup-harness.sh` extension (acknowledged required, not optional)**: `setup-harness.sh:111-131` hard-codes the three existing skills (`grill-with-docs`, `scratchpad-recall`, `agent-tokens`) by name. Adding `perf-instrument` / `perf-measure` requires extending `setup_claude_code()` — either by adding two new `link_dir` calls, or by replacing the hard-coded block with a loop over `agents/_shared/skills/*/SKILL.md`. v3 picks the loop approach (one-time refactor, future skills auto-link).
- **P2 — dependent frontmatter + AGENTS.md (addressed)**: `perf-detective.md:21` + `spike-hunter.md:20` carry `delegates-to: [perf-instrument, perf-measure]` frontmatter. Under dual-publish the frontmatter stays valid (the agents still exist). v3 adds a small prose note in both files telling the orchestrator to prefer the skill form on Claude Code. AGENTS.md `:19`, `:67`, `:380` pointers to the agent files stay valid.
- **P2 — verification buckets (addressed)**: per `docs/CONTEXT.md:63`, every plan's `## Verification` must classify each item into bucket A-E or carry an explicit deferred-automation entry. v2's verification was free-form manual smoke tests. v3 reclassifies per bucket (mostly A + B, with explicit deferred-automation for the items that can't be automated yet).

User authorized: conservative list only, double-check everything.

## Verified facts (v2 fact-check pass — still valid)

- **Agent count**: 24. Verified by `Glob agents/*.md` (excluding `README.md`).
- **Agent list**: `architect`, `build-doctor`, `code-review`, `coderabbit-triage`, `command-system`, `debug-detective`, `git-janitor`, `grid-engine`, `handoff-implementer`, `lua-binder`, `mcp-toolsmith`, `mechanic`, `offline-sync`, `p4-blame`, `perf-detective`, `perf-instrument`, `perf-measure`, `pr-iterator`, `security-review`, `spike-hunter`, `test-author`, `test-rig`, `tracker-backend`, `unreal-bridge`.
- **Skill count**: 3 SKILL.md manifests:
  - `agents/_shared/skills/grill-with-docs/SKILL.md`
  - `agents/_shared/skills/scratchpad-recall/SKILL.md`
  - `agents/_shared/token-tracking/SKILL.md` ← **inconsistent path** (not under `skills/`)
- **`perf-measure.md` frontmatter**: `complexity: low`, `read-only: true`, `model: sonnet`, `effort: low`.
- **`perf-instrument.md` frontmatter**: `complexity: low`, `read-only: false`, `model: haiku`, `effort: low`.
- **`code-review.md`** + **`security-review.md`**: "wraps /review" is rhetorical, not structural (both heavy investigators with 140-line checklists + parallel sub-tool dispatch). v2 rejection rationale stands.
- **`setup-harness.sh:111-131`** (`setup_claude_code`): hard-codes `grill-with-docs` + `scratchpad-recall` + `agent-tokens` skill links. No auto-discovery.
- **`setup-harness.sh:133-142`** (`setup_codex`): "Codex / OpenAI Agents reads AGENTS.md + agents/*.md directly per the agents.md spec — no local adapter required." No skill concept exists for Codex today.
- **`perf-detective.md:21`** + **`spike-hunter.md:20`**: `delegates-to: [perf-instrument, perf-measure]` frontmatter declares the helpers.
- **`AGENTS.md:19`**: `agents/perf-instrument.md` + `agents/perf-measure.md` referenced directly.
- **`AGENTS.md:67`**: helpers list cites both by name.
- **`AGENTS.md:380`**: `perf-instrument` cited in semantic-search exception.
- **`docs/CONTEXT.md:63`**: "Every plan's `## Verification` step must classify into one bucket" (A: CLI/unit · B: Scenario · C: Screenshot diff · D: Sanitizer · E: ImGui Test Engine).

## Decision criteria (unchanged from v2)

Codify in a separate `docs/agent-rules/AGENT-VS-SKILL.md` (out of scope here).

**Skill when ALL hold:**
- Deterministic procedure or rule-sheet (no investigation loop).
- Bounded context footprint (≤3 file reads, ≤2 file edits typical).
- No multi-round user-feedback or PR-comment loop.
- No parallel-dispatchable subtask spawn.
- Output is direct action / inline result, not a return-summary the main thread synthesizes.

**Agent when ANY hold:**
- Heavy exploration / context bloat risk.
- Multi-round loop.
- Isolated worktree / spawned-child orchestration.
- Parallel-dispatchable.
- Delegates to other agents.

**Cross-harness consideration (new)**: Codex + Cursor read `agents/*.md` directly per the agents.md spec — they have no skill concept today. Until the spec (or a per-harness adapter) covers skills, conversions that *delete* agent files break cross-harness discovery. Dual-publish (keep agent file, add SKILL.md alias) is the safe pattern. Pure conversion is only safe under an explicit "Claude-only" gate, which we are not taking.

## Conservative core — dual-publish (2)

| # | Agent | Action | Effort | Risk |
|---|---|---|---|---|
| 1 | `perf-measure` | Add SKILL.md alias at `agents/_shared/skills/perf-measure/SKILL.md` (Claude `@`-imports `../../../perf-measure.md`). Keep `agents/perf-measure.md`. Extend `setup-harness.sh setup_claude_code()` to loop-link all `agents/_shared/skills/*/`. | S (~1.5 hr) | Low |
| 2 | `perf-instrument` | Add SKILL.md alias at `agents/_shared/skills/perf-instrument/SKILL.md` (Claude `@`-imports `../../../perf-instrument.md`). Keep `agents/perf-instrument.md`. (Loop-link from #1 already covers it.) | S (~1 hr) | Low |

**Why these two and nothing else**: both are pure procedures (`perf-measure` read-only CLI + JSON parse; `perf-instrument` mechanical marker edits per spec). Both are sub-steps of heavier agents (`perf-detective` / `spike-hunter`). Inline skill matches actual usage shape on Claude Code; agent form preserved for Codex / Cursor.

**Why dual-publish rather than full conversion**: keeping the agent file is the only way to preserve cross-harness discovery today. The skill alias is the Claude-Code-specific affordance. When (and if) Codex / Cursor add skill discovery, the agent file can be evaluated for deletion as a follow-up.

## Cross-harness strategy (resolves P1#1)

**Per-harness behavior under dual-publish:**

| Harness | Reads | Invokes perf-instrument as |
|---|---|---|
| Claude Code | `.claude/agents/` (symlink → `agents/`) **and** `.claude/skills/` (symlink → `agents/_shared/skills/`) | **Skill** (lower context cost; orchestrator-side preference encoded in `perf-detective.md` + `spike-hunter.md` prose update) |
| Codex / OpenAI Agents | `AGENTS.md` + `agents/*.md` only | **Agent** (unchanged behavior) |
| Cursor | `.cursor/rules/agents.mdc` → `AGENTS.md` + `agents/` | **Agent** (unchanged behavior) |

**SKILL.md content** — minimal Claude-Code `@`-import wrapper, e.g. `agents/_shared/skills/perf-measure/SKILL.md`:

```markdown
---
name: perf-measure
description: <copy first line of perf-measure.md description>
triggers:
  - measure
  - snapshot
  - scenario
  - perf-run
---

@../../../perf-measure.md
```

Claude Code's `@`-import resolves the path at load time; the skill body becomes the agent body. Single source of truth (`agents/perf-measure.md`); SKILL.md is the alias.

**Orchestrator preference note** — small prose addition to `perf-detective.md` + `spike-hunter.md` (after the `delegates-to:` block in the existing prose):

> On Claude Code, prefer invoking `perf-instrument` and `perf-measure` as skills (lighter than subagent spawn). On Codex / Cursor, the agent form is the only option — invoke as documented in `delegates-to:`. Both forms read the same canonical content (`agents/perf-instrument.md`, `agents/perf-measure.md`).

No frontmatter changes — `delegates-to:` stays valid because the agent files stay.

## `setup-harness.sh` extension (resolves P1#2)

Replace `setup_claude_code()` lines 124-128 (hard-coded skill links) with a loop:

```bash
# Auto-link all SKILL.md packages under agents/_shared/skills/.
for skill_dir in agents/_shared/skills/*/; do
  skill_name="$(basename "$skill_dir")"
  link_dir ".claude/skills/$skill_name" "$skill_dir"
done

# Special case: token-tracking lives at a non-conforming path
# (agents/_shared/token-tracking/, not agents/_shared/skills/token-tracking/).
# Until that's normalised, link the single file by hand.
link_file ".claude/skills/agent-tokens/SKILL.md"   "agents/_shared/token-tracking/SKILL.md"
link_file ".claude/hooks/agent-token-log.py"       "agents/_shared/token-tracking/agent-token-log.py"
link_file ".claude/hooks/agents-statusline.py"     "agents/_shared/token-tracking/agents-statusline.py"
```

Effect:
- All existing skills (`grill-with-docs`, `scratchpad-recall`) continue to link via the loop.
- New skills under `agents/_shared/skills/perf-*/` link automatically.
- `token-tracking` keeps its hand-wired path until the location normalization (separate cleanup).
- Future skill additions need no `setup-harness.sh` edit.

**Idempotency**: `link_dir()` already short-circuits on existing link/dir. Loop is safe to re-run.

## Dependent doc + frontmatter updates (resolves P2#1)

| Location | Current state | Change |
|---|---|---|
| `agents/perf-detective.md:21` frontmatter `delegates-to:` | Lists `perf-instrument`, `perf-measure` as agent delegations | **No change** — agent files still exist under dual-publish |
| `agents/perf-detective.md` prose | No skill-form mention | **Add** orchestrator-preference note (see Cross-harness strategy above) |
| `agents/spike-hunter.md:20` frontmatter `delegates-to:` | Same | **No change** |
| `agents/spike-hunter.md` prose | Same | **Add** same note |
| `AGENTS.md:19` (Pillar 1 § Tools) | Direct link to `agents/perf-instrument.md` + `agents/perf-measure.md` | **No change** — files still exist; harness-aware reader can pick agent or skill form |
| `AGENTS.md:67` (UX Pillar agent-ownership table) | `helpers: perf-instrument, perf-measure` | **No change** — they remain helpers in agent form for cross-harness |
| `AGENTS.md:380` (Semantic-search exceptions) | "`perf-instrument` already use text-search" | **No change** — statement still true; agent prose unchanged |
| `docs/agent-rules/delegation.md` routing tables | Lists both as agents | **Add** a note that on Claude Code they have a skill form; the agent form is the cross-harness fallback. No row movement. |
| `docs/guides/perf-workflow.md` | References agent form | Verify references hold; update if any prose says "must spawn perf-instrument agent" rather than "invoke perf-instrument" |

Net diff: 2 prose additions (perf-detective + spike-hunter) + 1 small note in delegation.md + a verification pass on PERF_WORKFLOW.md. No frontmatter changes. No file deletions. AGENTS.md untouched in v3.

## Verification — bucket-classified (resolves P2#2)

Per `docs/CONTEXT.md:63`, each verification item classified into bucket A-E. Manual residue carries an explicit deferred-automation entry per `agents/test-author.md`.

| # | Verification item | Bucket | Notes |
|---|---|---|---|
| V1 | `setup-harness.sh claude-code` is idempotent + creates `.claude/skills/perf-measure/SKILL.md` + `.claude/skills/perf-instrument/SKILL.md` after the loop refactor; re-running produces zero diffs | **A** (bash assertion: `test -L .claude/skills/perf-measure && test -L .claude/skills/perf-instrument`) | Add to `scripts/dev/test-setup-harness.sh` (new file) auto-enrolled by `bash scripts/dev/test-all.sh` |
| V2 | `perf-measure` skill produces same JSON top-N rows as `perf-measure` agent against a known scenario | **B** (`scenario.run --name=grid_open_close --frames=60`) | Diff the two `perf.snapshot` outputs (skill-invoked vs agent-invoked) with `jq` over `.data.rows`. Both must agree |
| V3 | `perf-instrument` skill inserts `SMATCHET_UI_PERF_SCOPE("perf_temp:Foo")` at line 1 of a target function + adds `#include "UiPerfMonitor.h"` if missing | **A** (doctest under `tests/Source_Core/PerfInstrumentSkillFixture.test.cpp` is *not* viable — the skill writes files, not pure-logic. Instead: **bash assertion** that runs the skill against a fixture `.cpp` file under `tests/fixtures/perf-instrument/` and `diff`s against an expected post-state) | Add to `scripts/dev/test-perf-instrument-skill.sh` (new file) auto-enrolled by `test-all.sh` |
| V4 | Build still passes after a `perf-instrument` skill insert + after a cleanup pass | **A** (`cmake --build --preset ninja-iter-msvc --target SmatchetStandalone`) | Implicit in V3 fixture if the fixture includes a build step; otherwise a separate assertion |
| V5 | `perf-detective` correctly invokes the skill form on Claude Code + the agent form on Codex; both produce the same actionable hot-row list | **Manual residue** | **Deferred-automation entry**: requires cross-harness CI rig (Codex + Claude Code in lockstep). Currently no Smatchet CI runs Codex. File entry under `docs/backlog/agent-self-improvement/tooling.md`. Until then, single-harness verify on Claude Code is sufficient gate. |
| V6 | Token-cost delta — main-thread input-token cost per perf-round drops after conversion | **Manual residue** | **Deferred-automation entry**: the agent-token-log telemetry (`agents/_shared/token-tracking/`) tracks subagent invocations but does **not** currently track skill-invocation overhead. File entry under `docs/backlog/agent-self-improvement/tooling.md`: extend token telemetry to record skill loads. Until then, sample-measure on a known perf round + record one before/after pair in the PR description. |
| V7 | delegation.md + PERF_WORKFLOW.md references stay valid after edits | **A** (`rg 'perf-instrument' agents/ docs/` returns only matches consistent with dual-publish wording) | Add assertion to `scripts/dev/test-doc-consistency.sh` (existing or new) |

**Manual residue summary**: V5 + V6 are deferred-automation. Both get explicit entries in `docs/backlog/agent-self-improvement/tooling.md` as part of the conversion PR — per `agents/test-author.md`, manual residue without a backlog entry is a `test-author` failure.

## Critical files (touched by this conversion)

**New files:**
- `agents/_shared/skills/perf-instrument/SKILL.md`
- `agents/_shared/skills/perf-measure/SKILL.md`
- `scripts/dev/test-setup-harness.sh` (V1)
- `scripts/dev/test-perf-instrument-skill.sh` (V3 + V4)
- `tests/fixtures/perf-instrument/<input>.cpp` + `<expected>.cpp` (V3 fixture pair)

**Edited files:**
- `scripts/setup-harness.sh` — `setup_claude_code()` loop refactor
- `agents/perf-detective.md` — add orchestrator-preference prose note
- `agents/spike-hunter.md` — same
- `docs/agent-rules/delegation.md` — note skill-form availability on Claude Code
- `docs/guides/perf-workflow.md` — verify references; update prose if needed
- `docs/backlog/agent-self-improvement/tooling.md` — add V5 + V6 deferred-automation entries

**Unchanged (under dual-publish):**
- `agents/perf-instrument.md`
- `agents/perf-measure.md`
- `AGENTS.md`

## Rollout

**Phase 0** — `setup-harness.sh` loop refactor. Single PR. No behavioral change for existing skills; unblocks Phase 1+2. Verified by V1.

**Phase 1** — `perf-measure` skill alias + perf-detective/spike-hunter prose note + delegation.md note + tooling-backlog entries V5/V6. Single PR. Verified by V2 + V5 + V6.

**Phase 2** — `perf-instrument` skill alias + V3 fixture + V4 build verify + V7 doc-consistency assertion. Single PR. Gated on Phase 1 functional-parity verification.

No Phase 3. Re-evaluate aggressive list only after Phase 1+2 deliver measurable main-thread context savings — that re-evaluation is its own plan, not an extension.

## Out of scope

- Implementation (this doc is recommendation only).
- Skill-location normalization for `token-tracking` (move `agents/_shared/token-tracking/` → `agents/_shared/skills/token-tracking/`).
- Authoring `docs/agent-rules/AGENT-VS-SKILL.md` (criteria doc).
- Cross-harness skill discovery for Codex / Cursor (a precondition for any future *deletion* of the agent files; currently dual-publish sidesteps this).
- Token-telemetry extension to cover skill loads (V6's deferred-automation entry — separate work).
- Any agent in the rejected list (`code-review`, `security-review`, `mechanic`, `test-author`, `test-rig`, `command-system`, `lua-binder`, `mcp-toolsmith`, `unreal-bridge`, `coderabbit-triage`, `git-janitor`, `grid-engine`, `tracker-backend`, `offline-sync`, `p4-blame`, `architect`, `debug-detective`, `perf-detective`, `spike-hunter`, `pr-iterator`, `handoff-implementer`, `build-doctor`).
