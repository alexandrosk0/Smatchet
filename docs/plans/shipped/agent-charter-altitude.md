# Plan — Agent charter altitude (operating-principle preamble + context budget)

> **Slug**: `agent-charter-altitude` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

Comparison against **ECC** (`affaan-m/ECC`) surfaced two cheap charter steals Smatchet lacks:

- **A short operating-principle preamble.** ECC opens with 5 named Core Principles before its rules. Smatchet's `AGENTS.md` jumps straight into the 4 **UX Pillars** (which are quality *targets* — perf / no-freeze / no-crash / a11y) and then a 224-line rules wall. There's no altitude layer naming *how agents operate* (autonomous, gated, delegating). A first-time reader (human or a freshly-onboarded harness) has no map.
- **A context-budget rule.** ECC: *"avoid the last 20% of the context window for large refactors; single edits tolerate higher utilization."* Smatchet tracks tokens (`agents/_shared/token-tracking/`) and notes context *isolation* as the delegation token-win (`docs/agent-rules/delegation.md:345`) but has **no rule on per-task context *utilization*** — when to spawn a fresh-context sub-agent before quality degrades.

A third ECC steal — **skills-first surface policy** (`skills/` canonical, `commands/` legacy-shim) — is **N/A**: Smatchet has **no commands surface** (`git ls-files` finds none; `agents/_shared/skills/` is the only workflow surface). Smatchet is already skills-only by construction; nothing to deprecate.

**Intended outcome — one sentence:** after this lands, `AGENTS.md` opens with a ≤15-line **5-principle operating preamble** that indexes the existing deep sections (ship-loops / merge-gates / delegation / plan-doc / self-improvement), and `delegation.md` carries an explicit **context-budget-by-task-class** rule — both *altitude over existing content*, not new behavior.

## Approach

Two small, additive edits, framed as **altitude, not bulk**.

**Preamble.** Add a § Operating principles block at the top of `AGENTS.md` (before § UX Pillars). Five one-line principles, each linking the section that already defines it — so the preamble is a *skimmable map* over the 224-line wall, not new rules. This is the explicit answer to the "AGENTS.md is a wall" critique: a reader greps the principle, follows the link, skips the rest. The 4 UX Pillars (quality targets) stay; the 5 principles (operating model) are a distinct axis above them.

Proposed five (distilled from existing sections, not invented):
1. **Autonomous by default** — run the ship-loop end-to-end; pause only on the defined exceptions. (§ Autonomous ship-loop default)
2. **Gate, don't trust** — every invariant is code-enforced (merge-gates, delta-lint, selftests), never a prose promise. (§ Merge gates, § Tiered enforcement)
3. **Delegate to specialists** — orchestrator routes to `agents/`; semantic-search before text-search. (§ Delegation, § Semantic codebase search)
4. **Plan before ship** — non-trivial work gets a plan-doc + `grill-with-docs` stress-test. (§ Process rules § Plan-doc family)
5. **Self-tighten** — every agent ends with `## Self-improvement`; friction becomes prompt patches. (§ Self-improvement loop)

**Context budget.** Add a rule to `delegation.md`, adjacent to the existing context-isolation note (line 345): for large multi-file / cross-subsystem work, keep the orchestrator's own context under ~80% utilization — spawn a fresh-context sub-agent for sub-scopes *before* the last 20%, where instruction-following degrades. Low-sensitivity work (single-file edits, docs, mechanical renames) tolerates higher utilization. Ties the new rule to the mechanism Smatchet already has (delegation = the fresh-context lever; token-tracking = the gauge).

**Net-length discipline:** preamble ≤ 15 lines, budget rule ≤ 8 lines. The preamble is justified *only* as a navigation aid — if it ever accretes rule-detail, it has failed; a note says so.

## Files to modify

1. `AGENTS.md` (edit) — add § Operating principles (≤15 lines, 5 linked one-liners) above § UX Pillars. Each line cross-links its canonical section; carries a "navigation only — no rule detail here" guard note.
2. `docs/agent-rules/delegation.md` (edit, near line 345) — add § Context budget by task class (≤8 lines): ~80% utilization ceiling for large refactors → delegate before the last 20%; high tolerance for low-sensitivity edits.
3. `AGENTS.md` § Delegation stub (edit) — one-line pointer to the new `delegation.md` § Context budget (the AGENTS.md delegation index already lists moved subsections; add this one).

## Existing utilities reused

- `docs/agent-rules/delegation.md` context-isolation note (line 345) — the new budget rule extends it, doesn't restate it.
- `agents/_shared/token-tracking/` — the existing per-agent token gauge the budget rule references as the "how do I know I'm near budget" signal.
- The AGENTS.md § Delegation moved-subsection index pattern — the new context-budget stub follows the same "land here, follow the cross-link" shape.
- `agents/scripts/core/test-markdown-links.sh` / `test-doc-anchors.sh` — validate the 5 preamble cross-links + the new stub link resolve.

## UX Pillar callouts

- **Pillar 1 (perf)** / **Pillar 2 (no-freeze)** / **Pillar 3 (never-crash)** / **Pillar 4 (a11y)**: no impact — pure prose edits to two Markdown files, zero runtime code.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

N/A — pure-docs. Only `AGENTS.md` + `docs/agent-rules/delegation.md` change; both `*.md`, so `is-pure-docs-diff.sh` returns true → build/ctest/perf gates skip.

## Risks / non-goals

**Risks:**
- **Preamble becomes bloat** — the exact thing the comparison criticized. → ≤15-line cap + an explicit "navigation only, no rule detail" guard note in the section itself; cross-links keep detail in the canonical sections. If a reviewer sees rule-detail migrating into the preamble, that's a finding.
- **Principle drift** — a renamed/removed section orphans a preamble link. → the cross-links go through `test-markdown-links.sh` / `test-doc-anchors.sh` (doc-validation group), which fail on a dead anchor.
- **Five-vs-four confusion** (operating principles vs UX pillars). → the preamble states up front it's a *distinct axis* (how agents operate) from the UX Pillars (quality targets); no renumbering of pillars.

**Non-goals:**
- **Skills-first surface policy** — N/A (no commands surface; already skills-only). A one-line "skills are the only workflow surface" note, if wanted at all, folds into the `agent-kit-productization` plan's `USAGE.md` / `docs/STRUCTURE.md`, not here.
- Trimming the 224-line rules wall itself — separate "context-surface diet" effort; this plan only adds the *map*, doesn't shrink the territory.
- Adding new operating *rules* — the preamble indexes existing behavior only.

## Verification

- **Bucket A / Bucket E**: `N/A — no code.`
- **Doc integrity**: `test-markdown-links.sh` + `test-doc-anchors.sh` green — all 5 preamble cross-links + the AGENTS.md→delegation stub resolve to live anchors.
- **Pure-docs classification**: `is-pure-docs-diff.sh` returns true → `test-all.sh` build/ctest correctly skipped.
- **Length check**: preamble ≤ 15 lines, budget rule ≤ 8 lines (eyeball at review; no automated line-gate — the discipline lives in the guard note + reviewer).
- **Build gate**: `N/A — pure-docs.`
- **Manual residue**: none.

## Out of scope (flagged, not designed)

- **Coverage-floor graduation** (ECC steal #4) — separate plan `coverage-threshold-graduation`.
- **Kit version / manifest / USAGE** (ECC packaging steals) — `agent-kit-productization` plan.
- **Per-line-cap automation** on the preamble — not worth a gate for one section; reviewer holds the line.

## Implementation log

- Wave-1 slice of `agentic-harness-campaign`. `AGENTS.md` § Operating principles (12-line, 5 linked one-liners + navigation-only guard note) added above § UX Pillars; `docs/agent-rules/delegation.md` § Context budget by task class (~80% utilization ceiling, delegate-before-last-20%, low-sensitivity tolerance, token-tracking gauge) added after § Why split; AGENTS.md § Delegation moved-subsection index gained the § Context budget pointer.

## Deviations from plan

- None material. Preamble landed at 12 lines (plan cap ≤15); context-budget rule at ~6 lines (cap ≤8) — both inside budget.

## Verification (actual)

- `test-portable-purity` PASS · `test-agent-contract` 25/25 · `test-docs` 7/7 (doc-anchors + markdown-links resolve the 5 preamble cross-links + the new stub). Pure-docs — no build/ctest gate.
