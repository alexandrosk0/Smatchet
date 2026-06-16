# Plan — Harness audit suite (prove-first: token-footprint audit, then gated auditors)

> **Slug**: `harness-audit-suite` (matches this file's basename without `.md`).
>
> **Status**: `active` — not started (only the plan doc merged, #793).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.
>
> **Concept credit**: the four tool ideas below are inspired by the ECC harness-audit skills ([affaan-m/ECC](https://github.com/affaan-m/ECC), MIT). This is a **clean-room** adoption — ECC's `SKILL.md` / collector scripts were read for *concept understanding only*; nothing is copied byte-for-byte. Each shipped tool carries a one-line courtesy credit; there is **no** vendored file, **no** `NOTICE`, **no** `origin: ECC` frontmatter (none is owed when no copyrighted expression is reproduced — and foreign literals would trip `test-portable-purity` for nothing).

## Context

Smatchet's self-improvement loop is **manual on the extraction side**: agents end with `## Self-improvement`, the orchestrator accumulates friction in `docs/self-improvement/categories/*`, and a human/orchestrator hand-patches prompts and rules. Four ECC skill *concepts* automate exactly that extraction half — they audit the harness and emit structured findings:

- **token-footprint audit** — audits the *static, design-time token footprint of the harness itself* (how many tokens `AGENTS.md` + the agent prompts + the skills cost to *load*); flags bloat + token-savings. AGENTS.md is **245 lines today and still growing** — this is the concrete, immediate target.
- **skill/agent stocktake** — audits skills/commands for quality (the breadth pass; extends `subagent-eval`, today Phase-1 `code-review`-only, which is the depth pass).
- **agent-stack audit** — multi-layer agent-stack diagnostic (wrapper regression, memory pollution, hidden repair loops) — Smatchet *is* a ~24-agent autonomous-loop harness.
- **rules-distill** — scans skills/agents, extracts principles appearing in 2+ places, distils them into *candidate* rule entries.

Together they could form a **harness self-maintenance suite** that feeds the loop Smatchet already runs. But three of the four spawn subagents and are token-heavy on-demand tools, so this plan **proves the pattern on the cheapest one first** (see § Approach) rather than building all four speculatively.

Three adoption constraints shape the plan:

1. **Form (Smatchet's own rubric).** Per [`docs/agent-rules/AGENT-VS-SKILL.md`](../../agent-rules/AGENT-VS-SKILL.md), *"Agent when ANY hold: … parallel-dispatch / delegates-out."* The stocktake, rules-distill, and stack-audit concepts all **spawn subagents** → by Smatchet's rubric they are **agent-shaped, not skills**. Only the token-footprint audit (a bounded collector-script read-audit, no subagent spawn, bounded orchestrator context) is cleanly **skill-shaped**. ECC packages all four as skills; Smatchet re-forms them to its own rubric.

2. **Name collision — avoided by naming.** Smatchet already ships a **`context budget` rule** (`AGENTS.md:174`, `delegation.md:348`) meaning *runtime orchestrator window-fill* — "keep your own context under ~80% utilization, delegate before the last 20%." The ECC `context-budget` skill measures something **different and unrelated**: *static design-time footprint cost*. To avoid two distinct concepts sharing one name, the adopted skill is named **`harness-token-audit`** — `context-budget` stays reserved for the shipped runtime rule. The shared word in ECC's naming is incidental; there is **no** near-twin intent (runtime fill vs design-time footprint are different axes).

3. **Clean-room + portability.** The tools land in `agents/_shared/skills/` and `agents/core/` — portable dirs under `test-portable-purity`. Because the adoption is clean-room (concept-only, no vendored ECC literals — no `~/.claude/skills/<name>/`, no ECC `rules/` dir assumption, no `origin: ECC`), it introduces **zero** new foreign literals, so portability passes by construction.

**Intended outcome — one sentence:** after Phase 1 lands, Smatchet has `harness-token-audit` — a clean-room, portability-pure, one-line-credited skill that reports the harness's static token footprint and emits bloat findings into `docs/self-improvement/categories/*` — proving the whole adoption pattern on the smallest surface before any subagent-spawning auditor is built.

## Approach

**Prove-then-build.** Ship the single cheap, clearly-valuable, subagent-free tool first; gate the three expensive subagent-spawning auditors behind *demonstrated recurring need*. Speculatively building three rarely-run token-heavy auditors is exactly the over-investment the cheap auditor exists to detect — so it goes first and earns the rest.

**Re-form to Smatchet's rubric, don't copy ECC's packaging.** Apply `AGENT-VS-SKILL.md` per item: token-footprint audit → **skill** (`harness-token-audit`, bounded collector read-audit); the three subagent-spawning auditors → **agents** under `agents/core/` when/if built (with optional Claude-only skill aliases later, the dual-publish pattern). This is the first real exercise of the codified rubric — the adoption *demonstrates* it.

**Wire to the existing loop, don't parallel it.** The auditor emits into `docs/self-improvement/categories/{process,tooling,infra,…}` using the existing entry format (`AGENT_SELF_IMPROVEMENT.md` § Format — `date · agent · [category] · P<0-3>` + Details + Concrete next action; the substrate is append-only markdown, no schema). The suite is the automated *finder*; the established loop is the *applier*. No second backlog system.

**Clean-room + one-line credit.** Read ECC's skills for concept understanding; author fresh prompts + a fresh collector script against Smatchet's actual tree. No byte copying, no `NOTICE`, no per-file license header, no `origin: ECC` frontmatter — just a one-line "concept credit: ECC ([affaan-m/ECC](https://github.com/affaan-m/ECC), MIT)" in each tool's doc body.

## Files to modify

**Phase 1 — `harness-token-audit` skill (the only committed phase):**

1. `agents/_shared/skills/harness-token-audit/SKILL.md` (new, authored fresh) — scans `AGENTS.md` + `docs/agent-rules/*` + `agents/**` (and the skill/command corpus), reports a per-file token/line-cost table + the top bloat candidates, and emits findings in the self-improvement entry format. Carries the one-line ECC concept credit. Skill-only (no agent twin — bounded, orchestrator-inline, no cross-harness agent discovery needed).
2. Collector script under `agents/scripts/core/` (new, authored fresh — **not** ECC's `skills/<name>/scripts/` layout) — does the mechanical token/line counting so the skill's orchestrator-context footprint stays bounded (it runs the script + reads a table, not the whole tree). Goes through `test-shell-lint.sh`.
3. `agents/scripts/core/test-skill-vs-agent-parity.sh` (edit) — add `harness-token-audit` to `SKILL_ONLY_HELPERS` (precedents: `drain-memory`, `author-plan-doc`) so the parity guard doesn't demand a matching `agents/` file.
4. `AGENTS.md` § Self-improvement loop (edit, one line) — pointer: `harness-token-audit` is the automated static-footprint half of the extraction loop (suggestion-only; the orchestrator still applies). Distinct from the runtime `context budget` rule.

## Existing utilities reused

- `docs/agent-rules/AGENT-VS-SKILL.md` (from the `internal-procedure-skills` plan) — governs the skill-vs-agent form choice; `harness-token-audit` is the skill case, the deferred three are the agent case. This plan is the rubric's first application.
- `docs/self-improvement/categories/*` + `AGENT_SELF_IMPROVEMENT.md` § Format — the existing backlog + entry format the auditor emits into; no new system, no new schema.
- `agents/scripts/core/test-portable-purity.sh` + `docs/high-integrity/portable-purity-baseline.txt` — the guard the new files must pass (clean-room ⇒ passes by construction).
- `agents/scripts/core/test-skill-vs-agent-parity.sh` `SKILL_ONLY_HELPERS` — the registry that exempts a skill-only helper from the agent-twin requirement.
- `agents/scripts/core/setup-harness.sh` skill loop — auto-wires the new skill.
- `agents/_shared/token-tracking/` — the per-agent token gauge `harness-token-audit` reports against / complements (runtime accounting vs static footprint).
- `agents/scripts/core/is-pure-docs-diff.sh` — confirms the diff (`*.md` + `agents/scripts/**`) classifies pure-docs, so build/ctest/perf gates skip.

## UX Pillar callouts

- **Pillar 1–4**: no runtime impact — a skill prompt + one collector script + a one-line AGENTS.md edit. Zero product code. Indirectly serves Pillar 1 by surfacing harness context/token bloat that inflates every agent's load cost.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

N/A — no Source/Core code, no C++. The diff is `*.md` under `agents/` + a collector script under `agents/scripts/core/` — all pure-docs-allowlisted by `is-pure-docs-diff.sh` (`*.md` + `agents/scripts/**`) — so it classifies pure-docs and build/ctest/perf gates skip. Verification is shell-lint + the audit dry-run.

## Risks / non-goals

**Risks:**

- **Wrong form (skill vs agent).** Adopting a subagent-spawner as a *skill* (ECC's packaging) would violate Smatchet's own rubric. → only `harness-token-audit` is a skill (no spawn, bounded); the three spawners, if built, are agents. The reslice makes this moot for Phase 1 — the one shipped item is unambiguously skill-shaped.
- **Name collision** between the new skill and the shipped `context budget` runtime rule. → resolved by naming the skill `harness-token-audit`; `context-budget` stays the runtime rule. Stated up front, not discovered late.
- **Portability-guard failure** from vendored ECC literals. → clean-room introduces no foreign literals; run `test-portable-purity.sh` locally to confirm zero new baseline entries.
- **License over-claim.** Vendoring a `NOTICE` + `origin: ECC` for a *non-derivative* (concept-only) adoption would manufacture foreign literals and over-state the derivation. → clean-room + a one-line courtesy credit; no `NOTICE`, no per-file header.
- **Speculative over-build.** Building three rarely-run, token-heavy subagent auditors before proving the pattern. → prove-first reslice: ship `harness-token-audit`, run it, and gate the three agents behind demonstrated recurring need (§ Out of scope).
- **Duplicating `subagent-eval` / the self-improvement loop.** → the auditor emits into the existing categories in the existing format; the deferred `harness-stocktake` (when built) will call the existing eval, not reimplement it.

**Non-goals:**

- `codebase-onboarding` — Smatchet is already onboarded; its docs are richer than this would produce. Parked.
- `code-tour` — rejected: needs the VS Code CodeTour extension **and** its `file:line` anchors violate Smatchet's durable-by-construction no-line-anchor doc rule.
- Auto-applying any audit finding — suggestion-only; the apply stays in the human/orchestrator loop.
- Other-stack ECC skills (django/kotlin/rust/healthcare/crypto/…) — out of scope by definition.

## Verification

- **Bucket A / E**: N/A — no code.
- **Form check**: `harness-token-audit` lives under `agents/_shared/skills/` and is registered in `SKILL_ONLY_HELPERS`; `test-skill-vs-agent-parity.sh` green.
- **Portability**: `bash agents/scripts/core/test-portable-purity.sh` introduces no new baseline entries (clean-room ⇒ zero foreign literals).
- **License**: one-line ECC concept credit present in the skill body; no `NOTICE`, no `origin: ECC` frontmatter.
- **Audit dry-run**: `harness-token-audit` emits a real per-file token-footprint table over the live tree and produces ≥1 well-formed `docs/self-improvement/categories/<cat>.md` entry (exact § Format shape), **not** a direct `AGENTS.md` write.
- **Shell lint**: `test-shell-lint.sh` on the new collector script.
- **Loop integration**: a sample finding lands as a well-formed self-improvement entry (date · agent · `[category]` · `P<0-3>` + Details + Concrete next action).
- **Build gate**: N/A — pure-docs (`is-pure-docs-diff.sh`).
- **Manual residue**: the audit is on-demand (not CI-wired); running it is a documented maintenance action in `AGENT_SELF_IMPROVEMENT.md`, not silent.

## Out of scope (flagged, not designed) — gated next phases

The three subagent-spawning auditors are **deferred, not cancelled** — each is independently valuable and independently buildable as an `agents/core/` **agent** (per the rubric) **once `harness-token-audit` has demonstrated the adoption pattern earns its keep** (its own first finding may well be "this suite is expensive — run it monthly"):

- **`agent-stack-audit`** (agent; from ECC `agent-architecture-audit`) — multi-layer diagnostic retargeted to the Smatchet agent fleet + ship-loop + merge-watcher; severity-ranked findings → self-improvement categories.
- **`harness-stocktake`** (agent; from ECC `skill-stocktake`) — breadth skill/agent-quality audit; **calls** the existing `subagent-eval` (`scripts/dev/agent-eval-run.sh` / `agent-eval-score.py`) where they overlap, doesn't fork it; findings → self-improvement.
- **`rules-distill`** (agent; from ECC `rules-distill`) — scans `AGENTS.md` headings + `docs/agent-rules/*` (NOT an ECC `rules/` dir); emits *candidate* rules → `docs/self-improvement/categories/process.md`, **never** auto-writes `AGENTS.md` (a self-modifying rule-writer is too sharp without the loop's review gate).

Also out of scope:

- **Claude-only skill aliases** (dual-publish) for the three audit agents — a later affordance once the agent forms prove out.
- **CI-scheduling the audits** (e.g. monthly `rules-distill` run) — evaluate after the on-demand forms land.
- **`codebase-onboarding` as a subsystem-doc generator** template — parked.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
