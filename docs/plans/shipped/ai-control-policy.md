# Plan — AI control policy (human authority, loop modes, escalate-don't-assume)

> **Slug**: `ai-control-policy` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

Smatchet has a rich *operating* contract (autonomous ship-loop, merge-gates, the 2026-05-30 action-biased standing default) but **no governance charter** stating who is in control and when the agent must stop. Ghostty separates `AI_POLICY.md` (governance — who may drive, what's required of them) from `AGENTS.md` (instruction — how to build); Smatchet has only the instruction half. The comparison flagged this as the one governance gap.

The needed policy is **not** Ghostty's (which governs outside human contributors using AI). Smatchet's governs the **autonomous agents' relationship to human control** — because this is a project where humans must stay in control of **quality and cost**. Three requirements from the maintainer:

1. **Two operating models, human-selected** — agents run *human-on-the-loop* (agent acts, human monitors and can interrupt) **or** *human-in-the-loop* (agent pauses at decision points for human input), **as requested**. Today only one mode is encoded (`ship-loops.md:116` — action-biased, on-the-loop).
2. **Prerelease default = human-in-the-loop after an approved plan.** During prerelease dev (now), the maintainer mostly authorises a *plan*, then has the agent execute it — pausing rather than improvising beyond the plan.
3. **Escalate, don't assume.** When a request **cannot be autonomously validated** — ambiguous spec, no gate/test to confirm correctness, irreversible-and-unauthorised, or cost-unbounded — the agent **stops and escalates**, never guesses. This generalises the existing exception (4) "anything not durably authorised" into a *validation* principle and adds a *cost* dimension the contract lacks.

**Intended outcome — one sentence:** after this lands, `AI_POLICY.md` is the human-authority charter — defining the two loop modes (human-set, prerelease-default in-the-loop), the escalate-when-unvalidatable invariant, and human control of cost — and the ship-loop contract is reconciled to reference it without contradiction.

## Approach

A short root `AI_POLICY.md` (Ghostty's governance-separated-from-instruction placement) sitting **above** the operating contract, plus the minimal reconciling edits so the existing autonomous-default doesn't contradict it.

**The charter (`AI_POLICY.md`) states:**
- **Authority.** Humans own quality + cost. Agent autonomy is a *granted, revocable mode*, not a default right. Everything the agent does is auditable (the existing PR / commit / `## Self-improvement` / postmortem trail) so the human can always reconstruct what shipped and why.
- **Two loop modes (human-selected via `SMATCHET_LOOP_MODE`, surfaced at SessionStart):**
  - *human-on-the-loop* — the action-biased mode already defined at `ship-loops.md:116` (commit/push/PR autonomously; decide reversible forks with a default + surface them; pause only on the enumerated exceptions). This becomes the explicit definition of "on-the-loop".
  - *human-in-the-loop* — execute only within an **approved plan**; pause at each decision point not covered by the plan; do not improvise scope. **Prerelease default.**
  Mode is set per session/task; absent an explicit setting in prerelease, assume **in-the-loop-after-plan**.
- **Escalate, don't assume (invariant in BOTH modes).** Before acting, the agent must be able to *autonomously validate* the action — a gate/test/spec confirms correctness, the scope is authorised, the cost is bounded. If it cannot, it **escalates** via `AskUserQuestion` with the blocker named; it never fills the gap with an assumption. This adds ship-loop pause-exception **(6) cannot-autonomously-validate / cost-unbounded**.
- **Cost control.** Token/compute spend is a human-governed budget (gauged by `agents/_shared/token-tracking/`). The agent surfaces cost and escalates *before* an unbounded or expensive autonomous run — runaway spend without validation is forbidden.

**Reconciliation (the careful part).** The charter must not contradict `AGENTS.md` § Autonomous ship-loop default / `ship-loops.md:116` ("MUST NOT pause"). Resolution: the MUST-NOT-pause rule applies **within the active mode's authorised scope**; the escalate-when-unvalidatable invariant is a *new pause trigger that fires in both modes*, not a weakening of autonomy in on-the-loop mode. The 2026-05-30 standing default is re-expressed as "on-the-loop mode selected"; prerelease selects in-the-loop. No rule is deleted — they are placed under the mode spectrum the charter defines.

## Files to modify

1. `AI_POLICY.md` (new, repo root) — the charter per § Approach: Authority · Two loop modes (`SMATCHET_LOOP_MODE`, prerelease default in-the-loop) · Escalate-don't-assume invariant · Cost control · Auditability. Kept short (Ghostty's is ~40 lines); links into `AGENTS.md` for the operating mechanics.
2. `AGENTS.md` (edit) — (a) top-of-file pointer to `AI_POLICY.md` as the governance layer above the operating contract (dovetails with the `agent-charter-altitude` § Operating principles preamble if that lands first); (b) add ship-loop pause-exception **(6) cannot-autonomously-validate / cost-unbounded — escalate** to the "Loop pauses ONLY for" list (AGENTS.md § Autonomous ship-loop default).
3. `docs/agent-rules/ship-loops.md` (edit) — frame the `:116` standing default as the **on-the-loop** mode definition; add the **in-the-loop** mode (plan-gated, pause-at-undocumented-decision) + the prerelease default; cross-link `AI_POLICY.md`. Add the (6) escalation exception text + the cost-budget escalation.
4. `agents/scripts/core/clear-session-context.sh` (edit) — emit a `## === loop-mode: <on|in> ===` banner into `.session-context.md` at SessionStart from `SMATCHET_LOOP_MODE` (default `in` during prerelease), mirroring the existing `p4-mode ACTIVE` banner pattern (`AGENTS.md` § Autonomous ship-loop default). So the orchestrator sees the active mode every session.
5. `project.config.json` (edit) — add `loop_mode` defaults under a `governance` block (`default: in`, `policy: AI_POLICY.md`) so the mode + policy path are config-sourced.
6. `project.config.schema.json` (edit — **required**, not optional). The schema root is `additionalProperties: false`, so a new top-level `governance` block in `project.config.json` **fails validation unless the schema defines it first**. Add a `governance` object to the schema's `properties` (e.g. `loop_mode` enum `["in","on"]` default `"in"`; `policy` string default `"AI_POLICY.md"`; `additionalProperties: false` on the sub-object to match the file's convention). This is the gate the § Verification "validates against its schema" check exercises — without it that check fails.

## Existing utilities reused

- `docs/agent-rules/ship-loops.md:116` standing-default — becomes the on-the-loop mode definition; not rewritten, re-framed.
- `AGENTS.md` § Autonomous ship-loop default ship-loop pause-exception list — extended by one (the escalation exception), same structure.
- `SMATCHET_AGENT_VCS` / `SMATCHET_WATCH_ALL_PRS` env-var-opt-in pattern + the `clear-session-context.sh` SessionStart banner (the `p4-mode` banner, `AGENTS.md` § Autonomous ship-loop default) — the exact mechanism `SMATCHET_LOOP_MODE` copies.
- `agents/_shared/token-tracking/` — the cost gauge the cost-control clause references.
- `AskUserQuestion` (the orchestrator's escalation channel) + `delegation.md:16` (escalate-on-lock-overlap precedent) — the escalation mechanism the invariant invokes.
- The PR / commit / `## Self-improvement` / `postmortems.md` trail — the auditability the charter cites; nothing new built.

## UX Pillar callouts

- **Pillar 1–4**: no runtime impact — a governance doc + contract-reconciling edits + a SessionStart banner. Zero product code. Indirectly serves Pillar 1 (cost) and overall quality (escalation prevents wrong-assumption rework).

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

N/A — no Source/Core code, no C++. The diff is `*.md` + `project.config.json` + a SessionStart shell script under `agents/scripts/core/`. `*.md` + `agents/scripts/**` are pure-docs-allowlisted; `project.config.json` is not, so is-pure-docs-diff.sh returns false and test-all.sh runs in full — but with no compiled change the build/ctest/perf gates are no-ops. Verification is shell-lint + the banner check.

## Risks / non-goals

**Risks:**
- **Contradiction with the action-biased standing default** (`ship-loops.md:116`, `AGENTS.md` § Autonomous ship-loop default MUST-NOT-pause). → resolved by the mode spectrum: MUST-NOT-pause holds *within the active mode's authorised scope*; the standing default IS on-the-loop mode; prerelease selects in-the-loop. No existing rule deleted — re-placed under the charter. This is the plan's central design point and the `grill-with-docs` stress-test focus.
- **Over-restricting autonomy** (defeating the ship-loop's value). → escalate-don't-assume is scoped to the *un-validatable / cost-unbounded* case, not "pause on everything"; on-the-loop mode keeps full autonomy; reversible forks still resolve with a default + surface (the `:116` rule stays).
- **Mode ambiguity** (which mode is active?). → `SMATCHET_LOOP_MODE` + the SessionStart banner make it explicit every session; absent a setting in prerelease, the documented default is in-the-loop.
- **"Cannot validate" judgement drift** (agent rationalising an assumption as "validated"). → the charter ties validation to *concrete* signals — a gate/test/spec, authorised scope, bounded cost; absent one, it is by definition not autonomously validatable → escalate. The `gate-escape-postmortem` mechanism catches misjudgements after the fact.

**Non-goals:**
- Ghostty-style external-contributor disclosure / denounce-list — N/A while solo; revisit if the repo opens to outside contributions (same trigger as the `solo-merge-review-gate` ADR).
- A hard token-budget enforcement gate — the charter states human control + escalate-before-unbounded; an automated budget *ceiling* is a separate follow-up.
- Rewriting the ship-loop mechanics — only the governance framing + one new exception; the merge-gates / stages are untouched.
- AI-generated-media or content rules (Ghostty's) — irrelevant to a C++ app harness.

## Verification

- **Bucket A / E**: N/A — no code.
- **Banner**: with `SMATCHET_LOOP_MODE` unset, `bash agents/scripts/core/clear-session-context.sh` writes `## === loop-mode: in ===` (prerelease default); with `SMATCHET_LOOP_MODE=on` it writes `on`.
- **Reconciliation check**: `AGENTS.md` + `ship-loops.md` contain no surviving statement that contradicts the charter — grep for "MUST NOT" / "always autonomous" and confirm each is mode-scoped or exception-qualified.
- **Escalation exception present**: `AGENTS.md` § Autonomous ship-loop default "Loop pauses ONLY for" list includes the new (6) cannot-validate/cost item.
- **Doc integrity**: `test-markdown-links.sh` + `test-doc-anchors.sh` green — `AI_POLICY.md` ↔ `AGENTS.md` ↔ `ship-loops.md` cross-links resolve.
- **Config**: `project.config.json` validates against its schema with the new `governance` block.
- **Shell lint**: `test-shell-lint.sh` on the edited `clear-session-context.sh`.
- **Build gate**: N/A — no compile.
- **Manual residue**: none — `SMATCHET_LOOP_MODE` is operator-set, documented in `AI_POLICY.md`, surfaced by the banner.

## Out of scope (flagged, not designed)

- **Automated cost-ceiling enforcement** (hard token budget that halts the loop) — charter states the principle; the enforcing gate is a follow-up (pairs with `token-tracking`).
- **External-contributor governance** — deferred until the repo opens up (shared trigger with `solo-merge-review-gate`).
- **Per-task mode override UI** — `SMATCHET_LOOP_MODE` is session/env-scoped for now; finer-grained per-task selection is a later affordance.
- **The `agent-charter-altitude` § Operating principles preamble** — separate plan; this charter links to it but doesn't depend on it landing first (it adds its own top pointer if the preamble isn't there yet).

## Implementation log

- **Plan prep (PR #808 → folded into this PR):** fixed stale `AGENTS.md:28`/`:32` line refs → drift-proof `§ Autonomous ship-loop default` section refs; added `project.config.schema.json` as required Files item 6 (root is `additionalProperties:false` → a new `governance` block fails validation without a schema def).
- **Implementation (this PR):** all 6 files shipped.
  1. `AI_POLICY.md` (new, root) — charter: Authority · Two loop modes (`SMATCHET_LOOP_MODE`, prerelease default `in`) · Escalate-don't-assume invariant · Cost control · Scope.
  2. `AGENTS.md` — (a) top governance-layer pointer to `AI_POLICY.md`; (b) pause-exception **(6) cannot-autonomously-validate / cost-unbounded — escalate** appended to the "Loop pauses ONLY for" list + the in-the-loop plan-scope-pause note.
  3. `docs/agent-rules/ship-loops.md` — new **§ Loop modes (governance)** framing the standing default as the on-the-loop definition, adding in-the-loop (plan-gated, prerelease default), exception (6), cost-budget escalation, and the "MUST-NOT-pause holds within the active mode's authorised scope" reconciliation.
  4. `agents/scripts/core/clear-session-context.sh` — `## === loop-mode: <on|in> ===` SessionStart banner from `SMATCHET_LOOP_MODE` (default `in`; any non-`on` value normalises to `in`), mirroring the p4-mode banner.
  5. `project.config.json` — `governance` block (`policy: AI_POLICY.md`, `loop_mode: in`).
  6. `project.config.schema.json` — `governance` object property (loop_mode enum `["in","on"]`) so the config validates.
- **Recovery + attribution (PR #813):** PR #808's watcher squash-merged only the plan-prep commit before the feature commit landed (the #681 register-before-final-push trap, recurred), orphaning the implementation on the re-created branch. Recovered the orphaned commit, re-based onto develop, and re-shipped the full feature as #813. Added an `## Attribution` section to `AI_POLICY.md` crediting **Ghostty** (`ghostty-org/ghostty`, MIT) as prior art for the charter-separated-from-instruction pattern (structure borrowed, content original).

## Deviations from plan

- **No semantic deviation.** The plan-prep step (PR #808) was folded into this single feature PR rather than landing separately — same logical feature, and the plan-revision ships with the implementation per `AGENTS.md` § Process rules.
- AGENTS.md top-pointer (Files 2a) noted "if the `agent-charter-altitude` § Operating principles preamble lands first" — that § exists today, so the pointer sits as a blockquote directly under the intro, above § Operating principles (no dependency taken).

## Verification (actual)

- **Banner**: `SMATCHET_LOOP_MODE` unset → `## === loop-mode: in ===`; `=on` → `on`; garbage/`in` → `in` (fail-safe to the conservative mode). Verified via the isolated branch logic.
- **Schema**: `project.config.json` + `project.config.schema.json` both valid JSON; `governance` is a defined schema property and the config has no keys outside the schema → validates (root `additionalProperties:false` satisfied).
- **Reconciliation**: the sole surviving `MUST NOT ... pause` (`AGENTS.md` § Autonomous ship-loop default) now terminates in exception **(6)** + the in-the-loop plan-scope-pause clause → mode-scoped / exception-qualified, no unconditional contradiction. `AI_POLICY.md` + `ship-loops.md § Loop modes` carry the "within the active mode's authorised scope" framing.
- **Doc gates**: `md_lint` clean; `test-agent-contract` 25/0 (AGENTS.md structural gate intact after the top-pointer + exception-(6) edits).
- **Shell lint**: `test-shell-lint.sh` green on the edited `clear-session-context.sh`.
- **Build gate**: N/A — no C++.
