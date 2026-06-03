# Plan — Agentic-harness campaign (sequence the 6 ECC-comparison plans)

> **Slug**: `agentic-harness-campaign` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

Six plans authored from the ECC / Ghostty comparison sit unstarted in `docs/plans/active/`: `agent-charter-altitude`, `internal-procedure-skills`, `solo-merge-review-gate`, `coverage-threshold-graduation`, `agent-kit-productization`, `harness-audit-suite`. They share dependencies and a common substrate (the portable agent kit + `project.config.json` + the purity baseline), so shipping them blind, in filename order, would duplicate work (two context-budget artifacts) and hit hard blockers mid-flight (one plan needs a rubric another creates; one needs an external repo's source). This campaign is the **sequencing meta-plan** — it does not re-author the six; it orders them, records the grill's corrections, and draws the autonomous-vs-deferred line.

This doc is the campaign tracker, not a feature. Each child plan keeps its own slices + post-ship sections.

## Grill corrections (verified against the live tree 2026-06-02)

1. **`solo-merge-review-gate` premise is STALE.** Live `develop` protection already reports `required_pull_request_reviews: null` (0 required reviews) — the "solo deadlock" it describes is already resolved (confirmed: 7 PRs merged cleanly this session). Its core change (set review-count → 0) is **done**. Only the **codify-so-it-can't-drift** half stays valid: `setup-branch-protection.sh` + a `branch_protection` config block + an ADR. Child plan § Context must be rewritten to "lock in the current state," and the live-apply becomes an idempotent *confirmation*, not a fix.
2. **`harness-audit-suite` has no source.** No ECC content exists in-tree (`grep` for `affaan` / `agent-architecture-audit` / `skill-stocktake` / `rules-distill` finds only the plan docs). It cannot "port / adapt" ECC skills; doing it means native design from the plan's prose descriptions — heavy judgment work — and the MIT-derivative attribution is moot if nothing is copied. **Defer** until ECC source is available OR the plan is re-scoped to native-from-scratch.
3. **`coverage-threshold-graduation` is real + overdue but needs a decision.** `coverage.yml` is still advisory (`continue-on-error: true`, `--threshold 0`, soak date 2026-05-30 passed). Slice 1 (exclude `Source/Core/src/Ui/`) + the measurement run are mechanical, but the *flip threshold* is a data-driven maintainer call (70 if the real number clears it, else a floor + ramp). **Not fully autonomous** — defer the flip.

Verified TRUE (no correction): `AGENT-VS-SKILL` rubric exists verbatim in `docs/plans/shipped/agents-skill-conversion.md` § Decision criteria (liftable); `SKILL_ONLY_HELPERS` + the `drain-memory` precedent exist in `test-skill-vs-agent-parity.sh`; `setup-locks-ruleset.sh` is the idempotent-`gh api` setup-script precedent; purity baseline = 156 lines.

## Dependency graph

```
internal-procedure-skills ──(AGENT-VS-SKILL.md rubric)──▶ harness-audit-suite
agent-charter-altitude ──(context-budget rule cross-link)──▶ harness-audit-suite
agent-kit-productization Phase B (purity→0) ──(new files stay pure)──▶ harness-audit-suite
coverage-threshold-graduation ── independent
solo-merge-review-gate ── independent (re-scoped: codify-only)
```

`harness-audit-suite` is the sink of all three dependency edges — it ships LAST regardless of the ECC-source blocker. The context-budget concept exists twice on purpose: a **rule** (`agent-charter-altitude`, the guidance) and a **skill** (`harness-audit-suite`, the tool) — charter lands first so the skill cross-links it rather than duplicating.

## Approach — sequenced waves

**Wave 1 (autonomous, this campaign): the clean trio.** Dependency-leading, fully verifiable locally, no external deps, no judgment gate.
1. `agent-charter-altitude` — 2 doc edits (AGENTS.md § Operating principles preamble ≤15 lines + `delegation.md` § Context budget ≤8 lines). Feeds #6.
2. `internal-procedure-skills` — lift `AGENT-VS-SKILL.md` from the shipped plan + add the `author-plan-doc` skill (skill-only → `SKILL_ONLY_HELPERS`). **Keystone — unblocks #6.**
3. `solo-merge-review-gate` (re-scoped) — `setup-branch-protection.sh` + `branch_protection` config block + ADR; rewrite the child plan's stale § Context; the live-apply is a maintainer-run confirmation.

**Wave 2 (autonomous, next): packaging.**
4. `agent-kit-productization` Phase A — VERSION + CHANGELOG + manifest generator + selftest + USAGE + bats. Clean, large.

**Wave 3 (gated / incremental):**
5. `agent-kit-productization` Phase B — drive the 156-line purity baseline toward zero (mechanical sweep, per-file PRs). Couples to #6 (its new files must be pure-from-birth). Phase C is design-deferred by the child plan (gated on a real second consumer — do not build).
6. `coverage-threshold-graduation` — Slice 1 (Ui exclusion) + measurement run can be autonomous; the **flip + threshold number is a maintainer decision** → pause for the data + the call.
7. `harness-audit-suite` — blocked on ECC source (correction 2) AND depends on #2; LAST.

## Files to modify

This meta-plan authors no product files. Its only artifact is this tracker; the work lands in each child plan's write set. Per-wave, the child plans' § Files to modify govern.

- `docs/plans/active/solo-merge-review-gate.md` (edit, in the Wave-1 #3 PR) — rewrite § Context per correction 1.
- `docs/plans/active/agentic-harness-campaign.md` (this file) — § Implementation log updated as each wave ships.

## UX Pillar callouts

- **Pillar 1–4**: no runtime impact at the campaign level — every child plan is docs / agentic-shell / CI-config / project.config.json; none compiles a `Source/Core/` change. Per-child UX-pillar callouts live in each child plan.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

N/A at the campaign level — no `Source/Core/` source change in any wave. Each child plan re-asserts this in its own § Perf gate.

## Verification

- **Wave 1 acceptance:** the three child PRs merge green; `AGENT-VS-SKILL.md` resolves; `author-plan-doc` passes the parity guard; `setup-branch-protection.sh --dry-run` emits the desired object; doc gates (`test-docs` 7/7) green on each.
- **Per-child:** each child plan owns its § Verification; this campaign only tracks sequence + blocker state.

## Implementation log
*(populated post-ship — one bullet per shipped child wave)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
