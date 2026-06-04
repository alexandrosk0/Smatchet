# Plan — Reduce agent-prompt + AGENTS.md bloat (extract to skills + rule-docs, size gate)

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

**The goal is maintainability** — a 733-line agent prompt is hard for a human (or a model) to read, review, and edit safely; the size itself is the problem. (Runtime context-tax — the agent's `.md` loads on delegation — improves incidentally where an extracted skill is conditionally loaded, but that is **not** the objective and the plan claims no runtime number.) Inspection of the two whales shows the bloat is **not** reasoning — it's **deterministic procedure + embedded shell/code recipes** inlined in the prompt:

- `debug-detective` (733 L): a 12-step Debug Loop where §4 (roll a session-id helper + include + call sites), §7 (the exact log-grep command set), §12 (cleanup commands) are **verbatim recipes**. The genuinely agent-shaped part — hypothesis ranking, evidence/metric choice, the wait-for-feedback loop, hand-off, hard rules, report shape — is maybe ~300 L.
- `git-janitor` (534 L): path-resolution, the standard cleanup loop, stale-branch sweep, bring-`develop`-to-latest, poll-until-stable are deterministic **shell procedures**; the agent-shaped part is the refusal rules + the merge-gate orchestration reasoning.

Per [`docs/agent-rules/AGENT-VS-SKILL.md`](../../agent-rules/AGENT-VS-SKILL.md), a *deterministic procedure or rule-sheet with a bounded context footprint and no investigation loop* is a **skill**, not agent prose. The bloat is exactly that class of content living in the wrong place. Moving the recipes into skills leaves the agent as a thin, **readable** reasoning/routing layer — that readability is the win.

**Prior art (extend, don't duplicate):** [`docs/plans/shipped/agents-skill-conversion.md`](../shipped/agents-skill-conversion.md) converted **two whole** low-complexity agents (`perf-instrument`, `perf-measure`) to dual-published skills and established (a) the cross-harness constraint — Codex/Cursor read `agents/*.md` directly, have no skill concept — and (b) the `setup-harness.sh` loop-over-skills refactor. **This plan is a different lane:** it does not convert *whole* heavy agents (they have real reasoning loops → they stay agents); it extracts the *deterministic procedure-bodies out of* heavy agents into skills, and adds the **missing size gate** so the bloat can't regrow.

**Intended outcome — one sentence:** after this lands, heavy agents **and `AGENTS.md` itself** are **readable** — agents shrunk to their reasoning/routing core (procedure-bodies relocated to skills), `AGENTS.md` shrunk to its self-declared *navigation-only* role (rule-detail relocated to `docs/agent-rules/*`) — and a delta-gated **size budget over `agents/**/*.md` + `AGENTS.md` + `docs/agent-rules/*.md`** keeps them that way (the "gate, don't trust" guardrail none of these every-session-or-per-delegation docs currently have).

**`AGENTS.md` is also in scope, split by LOAD-FREQUENCY (added on review).** It is **255 lines** injected into **every** session — so the real waste is the **on-demand** content carried in always-loaded context: build recipes, the tiered-enforcement detail, debug techniques, golden-image, perf-workflow, p4/dual-VCS, the harness-adapter table — each needed only when *that* task type fires, dead weight otherwise. Its own § Operating principles already declares the target — *"Navigation only — no rule detail lives here."* The split axis is **always-needed (gates every task) vs on-demand (situational)**, not section-size; the on-demand set routes out to trigger-named docs (most already exist — `issue-triage.md`, `perf-workflow.md`, `golden-image-approval.md`, `process-rules.md`), leaving `AGENTS.md` a router + an every-edit invariant card. Smatchet already does this for perf (*"read `perf-workflow.md` **when** asked"*) — Slice 5 applies the lens systematically. Targets **255 → ~120 always-loaded**.

## Approach

**Gate-leads, like `decompose-top-20-monoliths`** — without the size gate the extraction decays under prompt-editing pressure (same erosion that plan proved for C++). Four slices.

**Slice 0 — agent-size gate (the keystone; ships first).** Add `agents/scripts/core/agent_size_audit.py` (sibling of `function_size_audit.py`): a **delta + grandfather** gate. The cap governs **new agents** and **growth of an existing agent past its baseline** — existing over-budget agents are snapshotted into `docs/high-integrity/agent-size-baseline.md` and **never fail regardless of size** (so the whales can be shrunk later without the gate fighting it; only *regrowth* past the snapshot fails). **Budget (locked, maintainer decision from the measured distribution — 11 agents ≤109 L, then a cliff to 133/141/214/268/534/733): hard cap 250 lines, soft-warn at 150** (mirrors the function-size 120-hard / 100-soft tiered shape). Lines, not bytes — line count is what reviewers reason about and what the function-size gate uses; a byte budget would penalise legitimately wide tables. Config-sourced (`project.config.json` § `agents.size_budget_lines` + `size_warn_lines`). Wire into `test-lint-rules.sh` + `test-all.sh` + CI; `SMATCHET_DEVIATION(rule=agent-too-long; …)` escape for a legitimately-large agent; `--selftest` asserts the budget matches this section. *Pure-logic Python; no `Source/` change → fast CI.* **The budget is NOT a Slice-1 dependency** — it is set here, now, from data already in hand.

**Gate scope = three file classes, three budgets** (the gate scans all three, delta+grandfather each):
- **`agents/**/*.md` (agent prompts):** hard 250 / soft 150 (above).
- **`AGENTS.md` (the every-session contract):** hard **150** / soft 120 — forces the route-out-the-on-demand-content trim (255 grandfathered; Slice 5 targets ~120 always-loaded).
- **`docs/agent-rules/*.md` (the extraction *sinks*):** **soft-warn only (≈400), no hard fail** — these are where AGENTS.md / agent detail *lands*, so a hard cap here would fight the extraction. The warn flags a rule-doc that itself grew monstrous (then split it), without blocking the relocation it's meant to receive.

**Slice 1 — classify + extraction map (no edits, just the readout).** For each over-baseline agent, tag every section per the rubric: **STAYS** (reasoning / multi-round loop / routing / refusal rules / report shape) vs **EXTRACT** (deterministic procedure + verbatim shell/code recipe), and flag each EXTRACT section **hot-path** (reached every invocation → stays inline, moving it just adds indirection) vs **conditional** (reached only sometimes → safe to extract). Record the per-agent before→after line estimate + destination skill. This **informs extraction targets**, not the gate budget (which is already locked in Slice 0).

**Slice 2 — pilot: `debug-detective` (the 733 L whale).** Extract the instrumentation recipe (§4 roll-session-id helper / includes / `LOG_*` fallback / rules), the log-reading command set (§7), and the cleanup commands (§12) into a skill — **`debug-instrument`** (a sibling of the existing `perf-instrument` skill, which already owns the analogous perf-marker recipe). The agent keeps: scope boundary, hypothesis ranking, evidence/metric choice, the §7.5 wait-for-feedback loop, crash/race workflows, hard rules, hand-off, report shape — plus a **one-paragraph summary + a pointer** to the skill (so the agent still *reads as complete* and Codex/Cursor get the what + a path to the full recipe). Target: 733 → ~300 L.

**Split principle (locked): the reasoning/recipe seam.** Extract strictly along the rubric line — the agent keeps the *judgment* (what you read to understand how it thinks: scope, hypotheses, evidence choice, loop, refusals, hand-off, report shape); the skill gets the *mechanics* (verbatim shell you copy-paste-run, rarely re-read for logic). Never split *mid-reasoning* (anything the agent's logic depends on inline stays). Each resulting file then does **one** thing. The agent carries an explicit named pointer (e.g. `Instrumentation recipe → agents/_shared/skills/debug-instrument/SKILL.md`) so there is zero "where did it go" hunting — the anti-scatter guarantee.

**Pilot-review gate (locked): STOP after Slice 2 for human review.** Before Slices 3-4 proceed, the maintainer reads the slimmed `debug-detective` and confirms it still reads as a **complete, self-explanatory thinker** (the split didn't gut comprehension). A failed review re-cuts the seam; a passed review unlocks the rollout. This is the visual-validation-style pause the maintainability goal demands — line count alone can't prove readability.

**Slice 3 — `git-janitor` (534 L).** Extract the deterministic VCS procedures (path resolution, standard cleanup loop, stale-branch sweep, bring-`develop`-to-latest, poll-until-stable) into a **`git-cleanup-procedures`** skill; the agent keeps the hard refusals + merge-gate orchestration reasoning + the FF-clean exception + a pointer. Target: 534 → ~250 L.

**Slice 4 — ride-along: `test-author` (268) + `coderabbit-triage` (214).** Same treatment **only if** a feature already opens the file or they exceed budget after Slices 0-3 calibration — not a dedicated sweep (the `decompose-monoliths` Phase-B lesson: mechanical sweeps churn + regress; the gate prevents regrowth so ride-along suffices).

**Slice 5 — split `AGENTS.md` by LOAD-FREQUENCY, not section-size (255 → ~120 always-loaded).** `AGENTS.md` is injected into **every** session's context, so the discriminator is **always-needed vs on-demand**, not "is this section long." Much of the file is *situational* — needed only when a specific task type fires (building, editing C++, handling an issue, debugging, p4-mode) — and is pure dead weight in the always-loaded context the rest of the time. The extraction test: **"would an agent need this on a task that ISN'T about that topic?"** No → on-demand → out.

- **5a — classify every `AGENTS.md` section ALWAYS vs ON-DEMAND** (the readout, mirrors Slice 1). *Always* = gates every task regardless of type: Operating principles, Quality Pillars (the invariant table), ship-loop default + loop modes, the Merge-gate summary, the few **every-edit** invariants (C++14 ban list, never-`printf`, the Don'ts), the `AI_POLICY.md` governance pointer. *On-demand* = situational: build recipes, the tiered-enforcement/zones/file-split/ImGui/`SMATCHET_DEVIATION` detail, debug techniques, golden-image, perf-workflow, dual-VCS/p4, harness-adapter table.
- **5b — extract the on-demand set into trigger-named docs**, each loaded only when its trigger fires (most already exist — reuse, don't recreate):

  | Trigger | Doc | New? |
  |---|---|---|
  | building | `docs/agent-rules/build.md` (presets · light-build · MSYS2-retired · dual-target · Unreal-lib clearing) | new |
  | editing C++ | `docs/agent-rules/cpp-rules.md` (quality · file-split · ImGui pattern · **tiered-enforcement + zones** · `SMATCHET_DEVIATION`) | new |
  | debugging | `docs/agent-rules/debug-techniques.md` (pink-clear · exe-staleness) | new |
  | handling an issue | `docs/agent-rules/issue-triage.md` | exists (#830) |
  | optimizing | `docs/guides/perf-workflow.md` | exists |
  | golden artefact | `docs/agent-rules/golden-image-approval.md` | exists |
  | p4-mode | `docs/perforce/AGENT_FLOWS.md` | exists |
  | plan/git lifecycle | `docs/agent-rules/process-rules.md` | exists |

- **5c — `AGENTS.md` becomes a router + invariant card**: the always-loaded core + a navigation index pointing at the trigger-docs. Target **~120 L always-loaded** (down from 255).

**Don't over-split** — group to the ~5-7 trigger-docs above, not 20 micro-files (each pointer is a hop an agent must follow; navigation thrash is its own bloat). **Anti-scatter:** explicit named pointer per topic. **Pilot-review gate:** maintainer reviews the routed `AGENTS.md` for "still navigates to everything + the always-card holds every-edit invariants" before merge (cross-harness: Codex/Cursor follow the same pointers). **Mechanically safe-guarded:** doc-validation's `test-doc-anchors` + `test-markdown-links` + `test-plan-ref-integrity` + `test-agent-contract` (all now *required*) fail CI if any `AGENTS.md §`-anchor / link / heading / plan-ref breaks in the move — keep extracted heading text stable or update referrers in the same PR (§ Scope-reduction grep).

## Cross-harness (the careful part — the design's central risk)

Skills are **Claude-Code-on-demand**; Codex/Cursor read `agents/*.md` **literally** and have **no skill concept** (`setup-harness.sh` § `setup_codex`). So extraction must not blind them. Mechanism decision (locked):

- **Extract the recipe into a skill, keep a compact summary + explicit file-path pointer in the agent.** The long verbatim shell/code blocks (the bulk of the bytes) move to `SKILL.md`; the agent retains the *what + when + "full recipe: `agents/_shared/skills/<name>/SKILL.md`"*. Claude loads the skill on demand (context saved); Codex/Cursor see the summary + a readable path (graceful degradation — they execute from the summary or open the file, rather than auto-injection). Register each skill-only extraction in `SKILL_ONLY_HELPERS` (`test-skill-vs-agent-parity.sh`) so the parity guard doesn't demand a whole-agent twin.
- **Rejected: `@`-import shared snippets** (`@agents/_shared/snippets/x.md`). Claude Code expands `@`-imports, but Codex/Cursor do **not** — they'd see a literal `@path` line and lose the content entirely. Worse cross-harness than the summary+pointer.
- **Rejected: dual-publish the whole procedure** (keep full recipe in both agent and skill). That's the prior plan's model for *whole-agent* conversion; here it would **not reduce** the agent at all (the bloat stays inlined for cross-harness) — defeating the goal.

Net: the Claude-context tax (the real cost — paid every invocation) drops; Codex/Cursor keep a working-but-terser path. This trade is flagged, not hidden, and is the `grill-with-docs` focus.

## Files to modify

1. `agents/scripts/core/agent_size_audit.py` (new) — delta-gated line budget over the **three classes** (`agents/**/*.md` hard 250, `AGENTS.md` hard 200, `docs/agent-rules/*.md` soft-warn-only ≈400); `--diff origin/develop`, `--selftest`, `SMATCHET_DEVIATION(rule=agent-too-long)` escape.
2. `docs/high-integrity/agent-size-baseline.md` (new) — grandfather snapshot of current over-budget files (the agent whales + `AGENTS.md` at 255).
3. `agents/scripts/project/test-lint-rules.sh` (edit) + `scripts/dev/test-all.sh` + the CI doc/lint job (edit) — run the new gate.
4. `project.config.json` + `project.config.schema.json` (edit) — a `governance`-sibling or `agents` sub-block carrying the three per-class budgets (`size_budget_lines`, `size_warn_lines`, the `AGENTS.md` + rule-doc caps) + schema def (root `additionalProperties:false`).
5. `docs/agent-rules/AGENT-VS-SKILL.md` (edit) — add the "**extract procedure-bodies, not just whole agents**" guidance + the reasoning/recipe seam + the summary+pointer cross-harness pattern. (This is the durable *why*-record — no ADR; the decision is reversible (re-inline + drop the gate) and the rubric is its natural home.)
5b. `AGENTS.md` § Project rules (edit) — one-liner for the new gate: "**Prompt/contract size**: `agents/**/*.md` ≤ **250 L**, `AGENTS.md` ≤ **200 L** (navigation-only), `docs/agent-rules/*.md` soft-warn ≈400; delta-gated by `agent_size_audit.py` vs `origin/develop`, existing over-cap files grandfathered (`docs/high-integrity/agent-size-baseline.md`), `SMATCHET_DEVIATION(rule=agent-too-long)` escape. Extract agent procedure-bodies to skills + `AGENTS.md` rule-detail to `docs/agent-rules/*` per `docs/agent-rules/AGENT-VS-SKILL.md`." So the gate is discoverable, not script-only.
   - **Extractions are full skills, not plain reference docs** (decided): the bodies are *invocable deterministic procedures* (the agent runs the instrument/cleanup step) — the rubric's skill definition, with `perf-instrument` as the exact precedent. A plain `_shared/snippets/` doc would invent a parallel un-guarded category; rejected.
6. `agents/core/debug-detective.md` (edit, Slice 2) — slim to reasoning core + pointer.
7. `agents/_shared/skills/debug-instrument/SKILL.md` (new, Slice 2) — the extracted instrument/log/cleanup recipe.
8. `agents/core/git-janitor.md` (edit, Slice 3) + `agents/_shared/skills/git-cleanup-procedures/SKILL.md` (new).
9. `agents/scripts/core/test-skill-vs-agent-parity.sh` (edit) — register the new skill-only helpers in `SKILL_ONLY_HELPERS`.
10. `agents/scripts/core/setup-harness.sh` — confirm the skills auto-link loop (landed by the prior conversion plan) picks the new skills up; no change expected, verify.
11. `AGENTS.md` (edit, Slice 5) — keep the always-loaded core (operating principles, pillars, ship-loop/loop-mode default, merge-gate summary, every-edit invariants, governance pointer) + a navigation index; route the on-demand content out. Target **255 → ~120**.
12. `docs/agent-rules/build.md` + `docs/agent-rules/cpp-rules.md` + `docs/agent-rules/debug-techniques.md` (new, Slice 5b) — the trigger-named extraction sinks for the building / editing-C++ / debugging on-demand content, structured like the existing `process-rules.md`. (issue-triage / perf-workflow / golden-image / p4 / process docs already exist — reuse, don't recreate.)

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
- **Skill-invocation overhead** — moot for the **maintainability** goal (a smaller agent file is the win regardless of when the skill loads); it would only matter if we were chasing a runtime-context number, which we are not. Slice 1 still tags hot-path vs conditional, but for a different reason: a **hot-path** procedure (reached every invocation, e.g. a rule the agent must always apply) stays inline because relocating a rule the agent always needs *hurts* readability (now you read two files for the always-case); only **conditional / standalone-recipe** sections (the long shell blocks the agent invokes occasionally) extract cleanly.
- **`AGENTS.md` extraction breaks an anchor / cross-ref** (it's the load-bearing contract; dozens of `AGENTS.md § <section>` refs + links point into it). → doc-validation already hard-gates this: `test-doc-anchors` (every `AGENTS.md §` resolves), `test-markdown-links` (no dangling link), `test-plan-ref-integrity`, `test-agent-contract` (required headings) — all **required** post the doc-validation-required fix. So a broken anchor/ref/heading in the move fails CI before merge; the Slice-5 extraction is mechanically safe-guarded, not trust-based. Keep the extracted section's **heading text stable** (or update all referrers in the same PR per the § Scope-reduction grep).

**Non-goals:**
- **Converting whole heavy agents to skills** — they have genuine reasoning loops; that's the prior plan's (closed) lane. This plan extracts *bodies*, keeps the agents.
- **Deleting any agent file** — cross-harness discovery depends on them; summary+pointer, never delete.
- **Touching the 11 already-lean agents** (≤ 109 L) — under any sane budget; the gate just keeps them there.
- **Shrinking the `docs/agent-rules/*` rule-docs** — they are the extraction **sinks** (AGENTS.md detail lands there), not shrink targets; gated soft-warn-only so they can absorb content. A rule-doc that itself becomes a monster gets split later, not now.
- **Rewriting any rule's *content*** — Slice 5 is byte-relocation of on-demand sections into trigger-named docs + a router; the rules themselves are unchanged (same behaviour-preserving discipline as the agent extraction).
- **Over-splitting into micro-files** — group to ~5-7 trigger-docs; a swarm of tiny docs is its own navigation-thrash bloat.
- **A per-harness skill adapter for Codex/Cursor** — the real cross-harness fix; separate follow-up, flagged.
- **Rewriting agent reasoning / behaviour** — extraction is byte-relocation + a pointer, behaviour-preserving (the `decompose-monoliths` discipline).

## Verification

- **Bucket A (pure-logic)**: `agent_size_audit.py --selftest` asserts the budget matches § Approach + the rubric classification rules; add a small fixture (an over-budget + an under-budget agent stub) asserting the delta gate fails the former, grandfathers the latter.
- **Gate behaviour**: a synthetic agent edit that grows a grandfathered agent past budget **fails** `test-lint-rules.sh --diff origin/develop`; an edit that *shrinks* it passes; a NEW over-budget agent fails.
- **Per-agent readout**: record before→after line counts (target debug-detective 733→~300, git-janitor 534→~250) in § Verification (actual).
- **Pilot-review gate (manual, blocking — Slice 2)**: maintainer reads the slimmed `debug-detective` and confirms it reads as a complete self-explanatory thinker + the seam didn't gut comprehension; PASS unlocks Slices 3-4, FAIL re-cuts the seam. Line count can't prove readability — a human reads it. Deferred-automation: the agent-eval harness (`scripts/dev/agent-eval-run.sh`, AGENTS.md § Subagent eval) can later score base-vs-slimmed agent quality, but the readability verdict stays human for now.
- **Behaviour-preserved (pilot)**: run a known debug task through the slimmed `debug-detective` + `debug-instrument` skill; confirm the loop still drives (instrument → build → run → read → hand-off).
- **Parity guard**: `test-skill-vs-agent-parity.sh` green with the new `SKILL_ONLY_HELPERS` registrations.
- **Cross-harness resolve**: `setup-harness.sh claude-code` + `codex` both still discover the slimmed agents + the new skills; the agent file still reads as self-contained (summary + pointer present).
- **`AGENTS.md` split (Slice 5)**: before→after line count recorded (target 255 → ~120 always-loaded, under the 150 cap); the always-card still holds every-edit invariants (C++14 ban, never-`printf`, the Don'ts) inline; every on-demand topic has a resolvable pointer (no orphaned content). `test-doc-anchors` + `test-markdown-links` + `test-agent-contract` + `test-plan-ref-integrity` all green (no `AGENTS.md §`-anchor / link / heading / plan-ref broken); maintainer pilot-reviews the routed `AGENTS.md` for "still navigates to everything + the always-card is complete."
- **Doc/config integrity**: `md_lint`, `test-agent-contract`, `project.config.json` validates against schema with the new size-budget block.
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
