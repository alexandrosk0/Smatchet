# Plan — Harness audit suite (adopt ECC self-maintenance skills)

> **Slug**: `harness-audit-suite` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

Smatchet's self-improvement loop is **manual on the extraction side**: agents end with `## Self-improvement`, the orchestrator accumulates friction in `docs/self-improvement/categories/*`, and a human/orchestrator hand-patches prompts and rules. Four ECC skills (MIT, © Affaan Mustafa) automate exactly that extraction half — they audit the harness and emit structured findings:

- **context-budget** — audits context-window consumption across agents/skills/MCP/rules; flags bloat + token-savings (directly the 224-line-AGENTS.md problem).
- **skill-stocktake** — audits skills/commands for quality (extends `subagent-eval`, today Phase-1 `code-review`-only).
- **agent-architecture-audit** — 12-layer agent-stack diagnostic (wrapper regression, memory pollution, hidden repair loops) — Smatchet *is* a 24-agent autonomous-loop harness.
- **rules-distill** — scans skills, extracts principles appearing in 2+ skills, distils them into rule files.

Together they form a **harness self-maintenance suite** that feeds the loop Smatchet already runs.

Two adoption frictions are load-bearing and shape the whole plan:
1. **Form (Smatchet's own rubric).** Per `docs/agent-rules/AGENT-VS-SKILL.md` (codified by the `internal-procedure-skills` plan), *"Agent when ANY hold: … parallel-dispatch / delegates-out."* **`skill-stocktake` ("sequential subagent batch evaluation"), `rules-distill` ("Launch a general-purpose Agent per batch"), and `agent-architecture-audit` (12-layer sweep) all spawn subagents** → by Smatchet's rubric they are **agent-shaped, not skills**. Only **context-budget** (a bounded read-audit) is cleanly skill-shaped. ECC ships all four as skills; Smatchet must re-form them to its own rubric.
2. **Layout + license.** These land in `agents/_shared/skills/` (or `agents/core/`) = a **portable dir** under `test-portable-purity`. The ECC originals carry foreign literals (`~/.claude/skills/<name>/scripts/…`, an ECC `rules/` directory that **does not exist here** — Smatchet rules live in `AGENTS.md` + `docs/agent-rules/*`) and `origin: ECC` frontmatter. They must be **de-ECC-ified + relayout-adapted**, and as MIT derivatives must **retain attribution**.

**Intended outcome — one sentence:** after this lands, Smatchet has a harness self-maintenance suite — `context-budget` (skill) + `harness-stocktake` / `agent-stack-audit` / `rules-distill` (agents) — adapted to its rule layout, attributed under MIT, and wired to emit into `docs/self-improvement/categories/*`.

## Approach

**Re-form to Smatchet's rubric, don't copy ECC's packaging.** Apply `AGENT-VS-SKILL.md` per item: `context-budget` → **skill** (bounded read-audit); the three subagent-spawning auditors → **agents** under `agents/core/` (with optional Claude-only skill aliases later, the dual-publish pattern). This is the first real exercise of the codified rubric — the adoption *demonstrates* it.

**Phase by adaptation cost.**
- **Phase 1 — `context-budget` skill + `agent-stack-audit` agent.** Lightest relayout: context-budget scans `AGENTS.md` + `docs/agent-rules/*` + `agents/{core,project,_shared}` and reports token-savings (it has a near-twin intent in the `agent-charter-altitude` plan's context-budget *rule* — the skill is the *tool*, the rule is the *guidance*; cross-link, don't duplicate). agent-stack-audit ports the 12-layer diagnostic onto the 24-agent fleet + ship-loop + merge-watcher.
- **Phase 2 — `harness-stocktake` agent.** Extends `subagent-eval` (don't fork it): stocktake is the breadth pass (all skills/agents), subagent-eval the depth pass (base-vs-head scoring). Wire stocktake to call the existing eval where they overlap.
- **Phase 3 — `rules-distill` agent (heaviest).** Its scan scripts assume an ECC `rules/` dir; rewrite the collectors for Smatchet's layout (`AGENTS.md` headings + `docs/agent-rules/*`). Output is *candidate* rules → routed to `docs/self-improvement/categories/*` for human/orchestrator apply, **not** auto-appended to `AGENTS.md` (a self-modifying rule-writer is too sharp without review).

**Wire to the existing loop, don't parallel it.** Every auditor emits into `docs/self-improvement/categories/{process,tooling,infra,…}` using the existing entry format — the suite is the automated *finder*; the established loop is the *applier*. No second backlog system.

**De-ECC-ify + attribute.** Strip foreign paths/literals (portable-purity must pass), retarget collectors to Smatchet's tree, keep `origin: ECC` + add `agents/_shared/skills/NOTICE` (or per-file header) carrying the MIT notice + © Affaan Mustafa for the derived files.

## Files to modify

**Phase 1:**
1. `agents/_shared/skills/context-budget/SKILL.md` (new, adapted) + its collector script under `agents/scripts/core/` (Smatchet's script home — **not** ECC's `skills/<name>/scripts/` layout; relocating is part of de-ECC-ification) — scans `AGENTS.md` + `docs/agent-rules/*` + `agents/**`; emits a token-savings table. Skill-only → add to `SKILL_ONLY_HELPERS`.
2. `agents/core/agent-stack-audit.md` (new, adapted from ECC `agent-architecture-audit`) — 12-layer diagnostic retargeted to the Smatchet agent fleet + ship-loop + merge-watcher; severity-ranked findings → self-improvement categories.
3. `agents/_shared/skills/NOTICE` (new) — MIT license text + © Affaan Mustafa attribution for all ECC-derived skills/agents (required by MIT).
4. `agents/scripts/core/test-skill-vs-agent-parity.sh` (edit) — `context-budget` → `SKILL_ONLY_HELPERS`; `agent-stack-audit` is an agent (has its own `.md`) so it satisfies parity natively.

**Phase 2:**
5. `agents/core/harness-stocktake.md` (new, adapted from ECC `skill-stocktake`) — breadth skill/agent-quality audit; calls `subagent-eval` (`AGENTS.md:88`) where they overlap; findings → self-improvement.

**Phase 3:**
6. `agents/core/rules-distill.md` (new, adapted from ECC `rules-distill`) + rewritten collectors under `agents/scripts/core/` — scan `AGENTS.md` headings + `docs/agent-rules/*` (NOT an ECC `rules/` dir); emit *candidate* rules → `docs/self-improvement/categories/process.md`, never auto-write `AGENTS.md`.

**Cross-cutting:**
7. `AGENTS.md` (edit) — § Self-improvement loop gains a one-line pointer: the audit suite is the automated extraction half (suggestion-only; the orchestrator still applies).
8. `docs/high-integrity/portable-purity-baseline.txt` — confirm no NEW literals introduced by the vendored files (run the guard; de-ECC-ify until clean).

## Existing utilities reused

- `docs/agent-rules/AGENT-VS-SKILL.md` (from the `internal-procedure-skills` plan) — governs the skill-vs-agent form choice for each adopted item; this plan is its first application.
- `docs/self-improvement/categories/*` + `AGENT_SELF_IMPROVEMENT.md` — the existing backlog the suite emits into; no new system.
- `agents/scripts/core/subagent-eval` path (`scripts/dev/agent-eval-run.sh` / `agent-eval-score.py`, `AGENTS.md:88`) — `harness-stocktake` calls it, doesn't reimplement.
- `agents/scripts/core/test-portable-purity.sh` + `docs/high-integrity/portable-purity-baseline.txt` — the guard the vendored files must pass.
- `agents/scripts/core/setup-harness.sh` skill loop + `agents/core/` agent discovery — auto-wire the new skill + agents.
- `agents/_shared/token-tracking/` — the gauge `context-budget` reports against.

## UX Pillar callouts

- **Pillar 1–4**: no runtime impact — agent/skill prompts + collector scripts + docs. Zero product code. Indirectly serves Pillar 1 by surfacing context/token bloat.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

N/A — no Source/Core code, no C++. The diff is `*.md` under `agents/` + `docs/`, plus collector scripts under `agents/scripts/core/` — all pure-docs-allowlisted by is-pure-docs-diff.sh (`*.md` + `agents/scripts/**`) — so it classifies pure-docs and build/ctest/perf gates skip. Verification is shell-lint + the audit dry-runs.

## Risks / non-goals

**Risks:**
- **Wrong form (skill vs agent).** Adopting the subagent-spawners as *skills* (ECC's packaging) would violate Smatchet's own rubric. → re-form per `AGENT-VS-SKILL.md`: only `context-budget` is a skill; the three spawners are agents. Stated up front, not discovered late.
- **Portability-guard failure** from vendored ECC literals (`~/.claude/skills/`, the `rules/` assumption, `origin: ECC`). → de-ECC-ify + retarget collectors to Smatchet's layout before commit; run `test-portable-purity.sh` locally; keep `origin` as attribution only.
- **MIT non-compliance.** Vendoring without notice violates the license. → `NOTICE` file with MIT text + © Affaan Mustafa, retained per derived file.
- **`rules-distill` auto-writing rules** unreviewed. → output is *candidates* into `docs/self-improvement/categories/*`; never direct-append to `AGENTS.md`. The orchestrator/human applies, preserving the loop's review gate.
- **Duplicating `subagent-eval` / the self-improvement loop.** → `harness-stocktake` calls the existing eval; all auditors emit into the existing categories. The suite *finds*, the loop *applies* — no parallel system.
- **Cost.** Four auditors that spawn subagents are token-heavy. → they are *on-demand* (periodic maintenance, not per-PR); `context-budget`'s own first finding may well be "this suite is expensive — run monthly."

**Non-goals:**
- `codebase-onboarding` — Smatchet is already onboarded; its docs are richer than this would produce. Parked.
- `code-tour` — rejected: needs the VS Code CodeTour extension (Claude/Codex can't render `.tour`) **and** its `file:line` anchors violate Smatchet's durable-by-construction no-line-anchor doc rule.
- Auto-applying any audit finding — suggestion-only; the apply stays in the human/orchestrator loop.
- Other-stack ECC skills (django/kotlin/rust/healthcare/crypto/…) — out of scope by definition.

## Verification

- **Bucket A / E**: N/A — no code.
- **Form check**: each adopted item's location matches its rubric verdict — `context-budget` under `agents/_shared/skills/`, the three spawners under `agents/core/`. `test-skill-vs-agent-parity.sh` green.
- **Portability**: `bash agents/scripts/core/test-portable-purity.sh` introduces no new baseline entries from the vendored files (de-ECC-ified clean).
- **License**: `NOTICE` present with MIT + attribution; each ECC-derived file references it.
- **Audit dry-runs**: `context-budget` emits a token-savings table over the real tree; `agent-stack-audit` / `harness-stocktake` / `rules-distill` each produce findings in the `docs/self-improvement/categories/*` entry format (not direct AGENTS.md writes).
- **Shell lint**: `test-shell-lint.sh` on every new collector script.
- **Loop integration**: a sample finding from each auditor lands as a well-formed `docs/self-improvement/categories/<cat>.md` entry.
- **Build gate**: N/A — pure-docs.
- **Manual residue**: the audits are on-demand (not CI-wired); running them is a maintenance action, documented in `AGENT_SELF_IMPROVEMENT.md`, not silent.

## Out of scope (flagged, not designed)

- **Claude-only skill aliases** for the three audit agents (dual-publish) — a later affordance once the agent forms prove out.
- **CI-scheduling the audits** (e.g. monthly `rules-distill` run) — evaluate after the on-demand forms land.
- **`codebase-onboarding` as a subsystem-doc generator** template — parked; revisit if the per-subsystem CONTEXT/README rollout needs a generator.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
