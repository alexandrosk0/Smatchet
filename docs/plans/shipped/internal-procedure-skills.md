# Plan — Internal procedure skills (codify AGENT-VS-SKILL + author-plan-doc)
<!-- plan-date: 2026-06-02 -->

> **Slug**: `internal-procedure-skills` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

Smatchet has a **documented skill-vs-agent rubric** — but it lives in a *shipped plan* (`docs/plans/shipped/agents-skill-conversion.md` § Decision criteria), not a live rule, and that plan explicitly said *"Codify in `docs/agent-rules/AGENT-VS-SKILL.md` (out of scope here)."* The file was never created. So every future skill/agent decision re-derives the criteria ad hoc.

Separately, the repo **under-uses skills for recurring orchestrator-inline procedures**. The most-repeated structured procedure in the project — authoring a plan-doc (`docs/plans/active/<slug>.md`) — has no skill. It lives as prose across `docs/plans/active/_plan-template.md` + `AGENTS.md` § Process rules + `docs/agent-rules/process-rules.md` (Plan-doc family / Plan-doc safety / Plan template). It passes the rubric cleanly (deterministic, bounded, no investigation loop, direct action) yet is hand-executed every time. `grill-with-docs` covers the *stress-test* of a plan, not its *authoring* — they are complementary.

**Intended outcome — one sentence:** after this lands, `docs/agent-rules/AGENT-VS-SKILL.md` is the live home of the skill-vs-agent rubric, and `author-plan-doc` is a skill that produces a template-conformant, immediately-committed plan from a one-line request.

## Approach

Two small additions, both net-new (no agent conversions).

**Codify the rubric.** Lift the criteria verbatim from the shipped conversion plan into `docs/agent-rules/AGENT-VS-SKILL.md` — the *"Skill when ALL hold / Agent when ANY hold"* lists + the cross-harness dual-publish note. This is a pure relocation of already-decided content into its intended home; the conversion plan named the exact path. Add a one-line pointer from `AGENTS.md`.

**Add `author-plan-doc`.** A skill encoding the plan-authoring procedure: copy `_plan-template.md` → set slug = basename → fill every section (`N/A — <reason>`, never delete — the forcing function) → include the Perf-review-system-gates section when the diff would touch `Source/Core/` → **`git add` + `wip(plan): <slug>` commit immediately** (plan-doc safety: working-tree-only plans are lost on checkout) → recommend a `grill-with-docs` stress-test before finalising → plan revisions are PR-only. It is **skill-only** (no agent twin) — orchestrator-inline, bounded, no isolation needed — so it registers in the parity guard's `SKILL_ONLY_HELPERS` (the `drain-memory` precedent).

The skill is the *authoring* counterpart to the *stress-test* skill `grill-with-docs`; together they bracket the plan lifecycle.

## Files to modify

1. `docs/agent-rules/AGENT-VS-SKILL.md` (new) — the rubric, lifted from `docs/plans/shipped/agents-skill-conversion.md` § Decision criteria: **Skill when ALL** (deterministic procedure; ≤3 reads/≤2 edits; no multi-round loop; no parallel-dispatch; direct action not return-summary) · **Agent when ANY** (heavy exploration; multi-round loop; isolated worktree; parallel-dispatch; delegates-out) · the cross-harness dual-publish pattern (keep the agent file for Codex/Cursor, add a Claude `@`-import SKILL.md alias).
2. `AGENTS.md` (edit) — one-line pointer to `AGENT-VS-SKILL.md` in the Delegation / agent-file-locations area.
3. `agents/_shared/skills/author-plan-doc/SKILL.md` (new) — frontmatter (`name`, `description`, `triggers: [write a plan, draft a plan, new plan, plan for, plan this]`) + workflow per § Approach. Read-write (creates + commits the plan file).
4. `agents/scripts/core/test-skill-vs-agent-parity.sh` (edit) — add `author-plan-doc` to `SKILL_ONLY_HELPERS` (skill with no agent twin; matches the `drain-memory` entry).

## Existing utilities reused

- `docs/plans/shipped/agents-skill-conversion.md` § Decision criteria — verbatim source of the rubric; no new criteria invented.
- `docs/plans/active/_plan-template.md` — the artifact `author-plan-doc` copies; the skill never authors blank.
- `docs/agent-rules/process-rules.md` § Plan-doc family / Plan-doc safety / Plan template — the rules the skill encodes (commit-immediately, N/A-not-delete, PR-only revisions).
- `agents/scripts/core/setup-harness.sh` (loop over `agents/_shared/skills/*/`, line ~223) — auto-links the new skill; **no setup change needed**.
- `agents/_shared/skills/grill-with-docs/SKILL.md` — the complementary stress-test skill the author-skill points to.
- `agents/_shared/skills/drain-memory` — the skill-only precedent for the `SKILL_ONLY_HELPERS` registration.

## UX Pillar callouts

- **Pillar 1–4**: no impact — docs + one skill manifest + a guard-array edit. Zero runtime code.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

N/A — no Source/Core code. The diff is `*.md` + `agents/scripts/**` only — both pure-docs-allowlisted by is-pure-docs-diff.sh — so it classifies pure-docs and build/ctest/perf gates skip.

## Risks / non-goals

**Risks:**
- **Rubric drift from its shipped-plan origin.** → it is a one-time lift; the shipped plan is archived (immutable), so there is no live second copy to diverge from. The new file is the single home going forward.
- **`author-plan-doc` skipping the commit-immediately step** (the exact failure plan-doc-safety guards). → the skill's workflow makes `git add` + `wip(plan)` a mandatory step, not optional; the `grill-with-docs` hand-off comes *after* the commit.
- **Parity guard fails on the new skill-only skill.** → mitigated by the `SKILL_ONLY_HELPERS` registration in the same PR; verification runs the guard.

**Non-goals:**
- Converting any agent to a skill — none here; per `AGENT-VS-SKILL.md` the current agents are correctly agents.
- `writing-commit-messages` + `scope-reduction-grep` skills — flagged below as follow-ups, not built here.
- Functional-parity testing between skill/agent forms — already a deferred stretch in the parity guard; unchanged.

## Verification

- **Bucket A / E**: N/A — no code.
- **Parity guard**: `bash agents/scripts/core/test-skill-vs-agent-parity.sh` green — `author-plan-doc` recognised as skill-only (in `SKILL_ONLY_HELPERS`), no orphan failure.
- **Auto-link**: `bash agents/scripts/core/setup-harness.sh claude-code` links `.claude/skills/author-plan-doc` via the existing loop; no hand-wiring.
- **Skill dry-run**: on a throwaway branch (`git checkout -b tmp/skill-test`), invoke `author-plan-doc` → it produces **and commits** a `docs/plans/active/<slug>.md` (exercising the commit-immediately step) that passes `test-plan-naming.sh` + `test-plan-ref-integrity.sh` and contains every required section (N/A-filled where N/A); then `git checkout develop && git branch -D tmp/skill-test` so the main line is untouched — the dry-run never pollutes `develop`.
- **Doc integrity**: `test-markdown-links.sh` + `test-doc-anchors.sh` green — the `AGENTS.md → AGENT-VS-SKILL.md` pointer resolves.
- **Pure-docs**: `is-pure-docs-diff.sh` returns true → build/ctest skipped.
- **Build gate**: N/A — pure-docs.
- **Manual residue**: none.

## Out of scope (flagged, not designed)

- **`writing-commit-messages` skill** — Smatchet's commit convention is AGENTS.md prose; a trigger-on-"write commit" skill (Ghostty has one) is a cheap follow-up.
- **`scope-reduction-grep` skill** — the AGENTS.md:119 final-check grep is a precise recurring procedure; skill-worthy follow-up.
- **`mechanic` dual-publish** — borderline (large renames exceed the ≤2-edit rubric); evaluate separately.
- **The audit-suite adoption** (context-budget / skill-stocktake / agent-architecture-audit / rules-distill) — separate plan `harness-audit-suite`.

## Implementation log

- Wave-1.2 of `agentic-harness-campaign`. `docs/agent-rules/AGENT-VS-SKILL.md` created — rubric lifted verbatim from `docs/plans/shipped/agents-skill-conversion.md` § Decision criteria (Skill-when-ALL / Agent-when-ANY + cross-harness dual-publish note + the `SKILL_ONLY_HELPERS` escape). `agents/_shared/skills/author-plan-doc/SKILL.md` created (frontmatter + 7-step workflow: derive slug → copy template → fill every section N/A-not-delete → mandatory Perf-gate when touching Source/Core → **commit-immediately** → grill hand-off → PR-only revisions). `agents/scripts/core/test-skill-vs-agent-parity.sh` registers `author-plan-doc` in `SKILL_ONLY_HELPERS`. `AGENTS.md` § Agent file locations gains the rubric pointer.

## Deviations from plan

- `setup-harness.sh` auto-links the new skill via its `agents/_shared/skills/*/` loop — no setup change needed, as the plan predicted.
- Bundled a pre-existing 3-line `docs/plans/INDEX.md` drift-fix (develop-side, unrelated) so this branch's `test-plan-index` gate passes.
- Noted (not fixed): `test-skill-vs-agent-parity.sh` has 3 PRE-EXISTING fails (`adversarial-code-review`, `but-for-real`, `drain-memory` — skills with no twin, not in `SKILL_ONLY_HELPERS`); out of scope, and the guard is advisory (not in the CI `test-docs` suite). `author-plan-doc` correctly SKIPs.

## Verification (actual)

- `author-plan-doc` → parity guard SKIP (intentionally skill-only). `test-portable-purity` PASS · `test-docs` 7/7 · `shellcheck` clean on the edited parity script. Pure-docs/agentic-shell — no build gate.
