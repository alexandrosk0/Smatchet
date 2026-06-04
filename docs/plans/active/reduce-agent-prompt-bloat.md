# Plan — Reduce agent-prompt bloat (extract procedure-bodies to skills + size gate)

> **Slug**: `reduce-agent-prompt-bloat` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

The `agents/core/` prompts have grown large and have **no size guardrail** — unlike first-party C++, which is gated on file size (67 KB) and function size (`function_size_audit.py`). Current measured state (17 core agents, ~2 992 lines):

| Agent | Lines | Bytes |
|---|---:|---:|
| `debug-detective` | **733** | 42 KB |
| `git-janitor` | **534** | 36 KB |
| `test-author` | 268 | 18 KB |
| `coderabbit-triage` | 214 | 18 KB |
| `security-review` | 141 | 10 KB |
| `code-review` | 133 | 9 KB |
| (11 others) | ≤ 109 each | — |

Every agent invocation loads its **entire** `.md` into context, so a 733-line agent is a 42 KB fixed tax on every debug task. Inspection of the two whales shows the bloat is **not** reasoning — it's **deterministic procedure + embedded shell/code recipes** inlined in the prompt:

- `debug-detective` (733 L): a 12-step Debug Loop where §4 (roll a session-id helper + include + call sites), §7 (the exact log-grep command set), §12 (cleanup commands) are **verbatim recipes**. The genuinely agent-shaped part — hypothesis ranking, evidence/metric choice, the wait-for-feedback loop, hand-off, hard rules, report shape — is maybe ~300 L.
- `git-janitor` (534 L): path-resolution, the standard cleanup loop, stale-branch sweep, bring-`develop`-to-latest, poll-until-stable are deterministic **shell procedures**; the agent-shaped part is the refusal rules + the merge-gate orchestration reasoning.

Per [`docs/agent-rules/AGENT-VS-SKILL.md`](../../agent-rules/AGENT-VS-SKILL.md), a *deterministic procedure or rule-sheet with a bounded context footprint and no investigation loop* is a **skill**, not agent prose. The bloat is exactly that class of content living in the wrong place. Skills load **on demand** (only when invoked); agent prose loads **every** invocation — so moving the recipes into skills cuts the always-paid context tax while keeping the agent the thin reasoning/routing layer.

**Prior art (extend, don't duplicate):** [`docs/plans/shipped/agents-skill-conversion.md`](../shipped/agents-skill-conversion.md) converted **two whole** low-complexity agents (`perf-instrument`, `perf-measure`) to dual-published skills and established (a) the cross-harness constraint — Codex/Cursor read `agents/*.md` directly, have no skill concept — and (b) the `setup-harness.sh` loop-over-skills refactor. **This plan is a different lane:** it does not convert *whole* heavy agents (they have real reasoning loops → they stay agents); it extracts the *deterministic procedure-bodies out of* heavy agents into skills, and adds the **missing size gate** so the bloat can't regrow.

**Intended outcome — one sentence:** after this lands, heavy agents shrink to their reasoning/routing core (procedure-bodies live in on-demand skills), and a delta-gated **agent-size budget** prevents regrowth — the "gate, don't trust" guardrail the agent tree currently lacks.

## Approach

**Gate-leads, like `decompose-top-20-monoliths`** — without the size gate the extraction decays under prompt-editing pressure (same erosion that plan proved for C++). Four slices.

**Slice 0 — agent-size gate (the keystone; ships first).** Add `agents/scripts/core/agent_size_audit.py` (sibling of `function_size_audit.py`): a **delta gate** — every NEW or **grown** `agents/**/*.md` over the budget fails vs `origin/develop`; existing over-budget agents are **grandfathered** into a baseline snapshot (`docs/high-integrity/agent-size-baseline.md`). Budget is **config-sourced** (`project.config.json` § `agents.size_budget_lines`, chosen from data — see Slice 1 readout; likely ~250 L hard / ~150 L soft-warn, mirroring the function-size tiered shape). Wire into `test-lint-rules.sh` + `test-all.sh` + CI, with a `SMATCHET_DEVIATION(rule=agent-too-long; …)` escape for a legitimately-large agent. `--selftest` asserts the budget matches this section. *Pure-logic Python; no `Source/` change → fast CI.*

**Slice 1 — classify + extraction map (no edits, just the readout).** For each agent > budget, tag every section per the rubric: **STAYS** (reasoning / multi-round loop / routing / refusal rules / report shape) vs **EXTRACT** (deterministic procedure + verbatim shell/code recipe). Record the per-agent before→after line estimate + the destination skill. This is the data the budget is set from.

**Slice 2 — pilot: `debug-detective` (the 733 L whale).** Extract the instrumentation recipe (§4 roll-session-id helper / includes / `LOG_*` fallback / rules), the log-reading command set (§7), and the cleanup commands (§12) into a skill — **`debug-instrument`** (a sibling of the existing `perf-instrument` skill, which already owns the analogous perf-marker recipe). The agent keeps: scope boundary, hypothesis ranking, evidence/metric choice, the §7.5 wait-for-feedback loop, crash/race workflows, hard rules, hand-off, report shape — plus a **one-paragraph summary + a pointer** to the skill (so the agent still *reads as complete* and Codex/Cursor get the what + a path to the full recipe). Target: 733 → ~300 L.

**Slice 3 — `git-janitor` (534 L).** Extract the deterministic VCS procedures (path resolution, standard cleanup loop, stale-branch sweep, bring-`develop`-to-latest, poll-until-stable) into a **`git-cleanup-procedures`** skill; the agent keeps the hard refusals + merge-gate orchestration reasoning + the FF-clean exception + a pointer. Target: 534 → ~250 L.

**Slice 4 — ride-along: `test-author` (268) + `coderabbit-triage` (214).** Same treatment **only if** a feature already opens the file or they exceed budget after Slices 0-3 calibration — not a dedicated sweep (the `decompose-monoliths` Phase-B lesson: mechanical sweeps churn + regress; the gate prevents regrowth so ride-along suffices).

## Cross-harness (the careful part — the design's central risk)

Skills are **Claude-Code-on-demand**; Codex/Cursor read `agents/*.md` **literally** and have **no skill concept** (`setup-harness.sh` § `setup_codex`). So extraction must not blind them. Mechanism decision (locked):

- **Extract the recipe into a skill, keep a compact summary + explicit file-path pointer in the agent.** The long verbatim shell/code blocks (the bulk of the bytes) move to `SKILL.md`; the agent retains the *what + when + "full recipe: `agents/_shared/skills/<name>/SKILL.md`"*. Claude loads the skill on demand (context saved); Codex/Cursor see the summary + a readable path (graceful degradation — they execute from the summary or open the file, rather than auto-injection). Register each skill-only extraction in `SKILL_ONLY_HELPERS` (`test-skill-vs-agent-parity.sh`) so the parity guard doesn't demand a whole-agent twin.
- **Rejected: `@`-import shared snippets** (`@agents/_shared/snippets/x.md`). Claude Code expands `@`-imports, but Codex/Cursor do **not** — they'd see a literal `@path` line and lose the content entirely. Worse cross-harness than the summary+pointer.
- **Rejected: dual-publish the whole procedure** (keep full recipe in both agent and skill). That's the prior plan's model for *whole-agent* conversion; here it would **not reduce** the agent at all (the bloat stays inlined for cross-harness) — defeating the goal.

Net: the Claude-context tax (the real cost — paid every invocation) drops; Codex/Cursor keep a working-but-terser path. This trade is flagged, not hidden, and is the `grill-with-docs` focus.

## Files to modify

1. `agents/scripts/core/agent_size_audit.py` (new) — delta-gated line/byte budget over `agents/**/*.md`; `--diff origin/develop`, `--selftest`, `SMATCHET_DEVIATION(rule=agent-too-long)` escape.
2. `docs/high-integrity/agent-size-baseline.md` (new) — grandfather snapshot of current over-budget agents.
3. `agents/scripts/project/test-lint-rules.sh` (edit) + `scripts/dev/test-all.sh` + the CI doc/lint job (edit) — run the new gate.
4. `project.config.json` + `project.config.schema.json` (edit) — `agents.size_budget_lines` (+ soft-warn) config + schema def (root is `additionalProperties:false`; the `agents` block already exists — add the field there + to its schema).
5. `docs/agent-rules/AGENT-VS-SKILL.md` (edit) — add the "**extract procedure-bodies, not just whole agents**" guidance + the summary+pointer cross-harness pattern.
6. `agents/core/debug-detective.md` (edit, Slice 2) — slim to reasoning core + pointer.
7. `agents/_shared/skills/debug-instrument/SKILL.md` (new, Slice 2) — the extracted instrument/log/cleanup recipe.
8. `agents/core/git-janitor.md` (edit, Slice 3) + `agents/_shared/skills/git-cleanup-procedures/SKILL.md` (new).
9. `agents/scripts/core/test-skill-vs-agent-parity.sh` (edit) — register the new skill-only helpers in `SKILL_ONLY_HELPERS`.
10. `agents/scripts/core/setup-harness.sh` — confirm the skills auto-link loop (landed by the prior conversion plan) picks the new skills up; no change expected, verify.

## Existing utilities reused

- `agents/scripts/core/function_size_audit.py` — the delta-gate-with-baseline pattern Slice 0 copies (keyed, grandfathered, `--selftest`, `SMATCHET_DEVIATION` escape).
- `docs/agent-rules/AGENT-VS-SKILL.md` rubric + `test-skill-vs-agent-parity.sh` `SKILL_ONLY_HELPERS` — the skill/agent boundary + parity guard.
- `agents/_shared/skills/perf-instrument/SKILL.md` — the precedent for an instrumentation-recipe skill that an agent (`perf-detective`/`spike-hunter`) invokes; `debug-instrument` mirrors it.
- `docs/plans/shipped/agents-skill-conversion.md` § cross-harness + the `setup-harness.sh` loop-over-skills refactor — the dual-publish/skill-link mechanism, already shipped.
- `decompose-top-20-monoliths` § Slice-0-leads + grandfather-baseline — the gate-first sequencing rationale.

## UX Pillar callouts

- **Pillars 1–4**: zero runtime / product-code impact — agent prompts, a Python gate, docs, config. Indirectly serves operating quality (leaner agents = more context budget for the actual task = fewer context-exhaustion handoffs).

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

N/A — no `Source/Core/` code, no C++. Diff is `agents/**/*.md` + a Python gate + `*.md` docs + `project.config.{json,schema.json}` + shell-gate wiring. `agents/**` + `*.md` are pure-docs-allowlisted; `project.config.json` is not, so `is-pure-docs-diff.sh` returns false and `test-all.sh` runs — but no `.cpp`/`.h` → build/ctest/perf gates are no-ops. Verification is the agent-size selftest + parity guard + shell/yaml lint + cross-harness resolve check.

## Risks / non-goals

**Risks:**
- **Cross-harness degradation** (Codex/Cursor lose the auto-injected recipe). → summary+pointer kept in the agent; they execute from the summary or read the named skill path. Flagged, accepted, `grill`-focus. A future per-harness skill-adapter closes it fully (separate plan).
- **Over-extraction guts the reasoning** (the agent stops being able to *think*, only *follow*). → extract **only** deterministic procedure + verbatim recipe per the rubric; the hypothesis loop / evidence choice / refusals / report shape **stay**. Slice 1's classification is reviewed before any edit; the pilot (Slice 2) is inspected for "does the slimmed agent still drive the loop" before Slice 3.
- **Gate too strict → red-bars agent edits** (the coverage-flip-blind lesson). → budget chosen **from Slice-1 data**, delta-gated + grandfathered (existing whales don't fail; only NEW growth does), generous soft-warn tier below the hard cap, `SMATCHET_DEVIATION` escape.
- **Skill-invocation overhead** (if the extracted procedure is needed on *every* invocation of the agent, moving it to a skill just adds an indirection without saving context). → only extract procedures that are **conditionally** reached (debug-instrument fires once per debug *that needs instrumentation*, not every turn); a procedure on the always-hot path stays inline. Slice 1 tags hot-path vs conditional.

**Non-goals:**
- **Converting whole heavy agents to skills** — they have genuine reasoning loops; that's the prior plan's (closed) lane. This plan extracts *bodies*, keeps the agents.
- **Deleting any agent file** — cross-harness discovery depends on them; summary+pointer, never delete.
- **Touching the 11 already-lean agents** (≤ 109 L) — under any sane budget; the gate just keeps them there.
- **A per-harness skill adapter for Codex/Cursor** — the real cross-harness fix; separate follow-up, flagged.
- **Rewriting agent reasoning / behaviour** — extraction is byte-relocation + a pointer, behaviour-preserving (the `decompose-monoliths` discipline).

## Verification

- **Bucket A (pure-logic)**: `agent_size_audit.py --selftest` asserts the budget matches § Approach + the rubric classification rules; add a small fixture (an over-budget + an under-budget agent stub) asserting the delta gate fails the former, grandfathers the latter.
- **Gate behaviour**: a synthetic agent edit that grows a grandfathered agent past budget **fails** `test-lint-rules.sh --diff origin/develop`; an edit that *shrinks* it passes; a NEW over-budget agent fails.
- **Per-agent readout**: record before→after line counts (target debug-detective 733→~300, git-janitor 534→~250) in § Verification (actual).
- **Behaviour-preserved (pilot)**: run a known debug task through the slimmed `debug-detective` + `debug-instrument` skill; confirm the loop still drives (instrument → build → run → read → hand-off) — manual, with a deferred-automation note (agent-eval harness, `scripts/dev/agent-eval-run.sh`, is the eventual automation per AGENTS.md § Subagent eval).
- **Parity guard**: `test-skill-vs-agent-parity.sh` green with the new `SKILL_ONLY_HELPERS` registrations.
- **Cross-harness resolve**: `setup-harness.sh claude-code` + `codex` both still discover the slimmed agents + the new skills; the agent file still reads as self-contained (summary + pointer present).
- **Doc/config integrity**: `md_lint`, `test-agent-contract`, `project.config.json` validates against schema with the new `agents.size_budget_lines`.
- **Build gate**: N/A — no compile.
- **Plan stress-test**: run `grill-with-docs` on this plan before finalising (AGENTS.md § Plan-doc family) and record the outcome.

## Out of scope (flagged, not designed)

- **Per-harness skill adapter** (Codex/Cursor native skill support) — the durable cross-harness fix; this plan accepts summary+pointer degradation until it lands.
- **Whole-agent → skill conversion** of any further agents — the prior plan's lane; reopen only with the same conservative + dual-publish discipline.
- **Auto-extraction tooling** (a script that proposes agent→skill splits) — manual classification (Slice 1) is the MVP; automate only if the ride-along set grows.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
