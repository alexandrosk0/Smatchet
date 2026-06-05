# Plan — Agent size reduction (shrink the 3 over-cap agents under 250)

> **Slug**: `agent-size-reduction` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

Split out of [`agent-audit-remediation`](agent-audit-remediation.md) § Out-of-scope (grill decision 2026-06-05). The portfolio eval found three core agents **over the 250-line `agent-too-long` hard cap**, grandfathered in [`agent-size-baseline.md`](../../high-integrity/agent-size-baseline.md) (so they pass the delta gate today, but with **zero headroom** — any additive edit trips it):

- `debug-detective` — **419** lines (already shrank 733 → 419 via the `debug-instrument` skill extraction; still the largest).
- `test-author` — **268** lines.
- `coderabbit-triage` — **215** lines (over the 150 soft-warn).

Split from the parent plan because skill-extraction is a **design call** (deciding the agent/skill seam per [`AGENT-VS-SKILL.md`](../../agent-rules/AGENT-VS-SKILL.md)), not the mechanical doc-fix the parent plan scoped, and because it has a **cross-plan dependency**: the parent's Slice 1 (PR #868) edits `test-author` + `coderabbit-triage` content, so this plan must sequence **after PR #868 merges** to avoid colliding edits.

**Intended outcome — one sentence:** after this lands, all three agents are under the 250 cap (target ~150 soft-warn for real headroom), their baseline entries are retired, and a contract check asserts the literal `## Self-improvement` trailing heading so the convention is machine-enforced.

## Approach

Extract procedure-bodies to skills (the proven pattern that already shrank `debug-detective` 733 → 419), leaving one-line judgement pointers in the agent. The judgement (when to act, which hypothesis, which metric) stays in the agent; the deterministic mechanics (checklists, bash skeletons, report templates) move to a skill the agent references.

Three independent shrinks + one contract-hardening, each verifiable via `agent_size_audit.py --diff origin/develop`:

- **debug-detective** → move the §8 Crash-specific and §9 Race/Ordering workflow checklists into the existing `debug-instrument` skill; leave one-line pointers.
- **test-author** → collapse the duplicated Report-format template into the Maintenance-class headings; carve a new `test-authoring` skill holding the bucket-E wire-up gotchas + the Pattern A/D bash skeletons.
- **coderabbit-triage** → de-duplicate the restated override rules; extract the per-finding handoff-packet template into a shared snippet if it can be shared.
- **contract-heading hygiene** → promote the inline `## Self-improvement` prose to a literal trailing heading in the 6 agents that inline it, and add a 14th check to `test-agent-contract.sh` asserting the literal heading (the parent plan revealed checks 1-4 already cover `## Outcome:` + per-class headings, so this is the only genuine gap).

## Files to modify

1. `agents/core/debug-detective.md` (419 → <250) — extract §8 / §9 checklists to `debug-instrument`; one-line pointers. Bump `version` + banner in lockstep (contract check 10).
2. `agents/_shared/skills/debug-instrument/SKILL.md` — receive the §8 / §9 checklists (the extraction sink; soft-warn-only on size).
3. `agents/core/test-author.md` (268 → ~150) — collapse the duplicated Report-format template; move bucket-E gotchas + Pattern A/D skeletons to the new skill.
4. `agents/_shared/skills/test-authoring/SKILL.md` (new) — the extracted test-authoring mechanics; `version:` field + sync-warning header per the skill convention; add to `SKILL_ONLY_HELPERS` in `test-skill-vs-agent-parity.sh`.
5. `agents/core/coderabbit-triage.md` (215 → <200) — de-duplicate restated override rules; extract the handoff-packet template.
6. Inline `## Self-improvement` → literal trailing heading in `mechanic`, `perf-measure`, `offline-sync`, `mcp-toolsmith`, `unreal-bridge`, `tracker-backend`.
7. `agents/scripts/core/test-agent-contract.sh` — add a 14th check: every `agents/{core,project}/*.md` ends with a literal `## Self-improvement` trailing heading.
8. `docs/high-integrity/agent-size-baseline.md` — **retire** the three grandfathered entries once each agent is under cap (residue-sweep owed by the parent plan).

## Existing utilities reused

- `agents/_shared/skills/debug-instrument/SKILL.md` — the existing extraction sink (debug-detective already points to it).
- `agents/scripts/core/agent_size_audit.py` — the `agent-too-long` gate that verifies under-cap.
- `agents/scripts/core/test-agent-contract.sh` (343 lines / 13 checks) — extended with the literal-`## Self-improvement` check; already asserts `## Outcome:` + per-class headings.
- `agents/scripts/core/test-skill-vs-agent-parity.sh` — `SKILL_ONLY_HELPERS` for the new `test-authoring` skill.
- [`AGENT-VS-SKILL.md`](../../agent-rules/AGENT-VS-SKILL.md) — governs the agent/skill seam for each extraction.

## UX Pillar callouts

- **Pillar 1-4**: no runtime impact — agent prompt + skill `*.md` + one shell check. Zero product code.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

**N/A** — no `Source/Core/` code. The diff is agent / skill `*.md` + `agents/scripts/**` (`is-pure-docs-diff.sh` pure-docs-allowlisted); build/ctest/perf gates skip.

## Risks / non-goals

**Risks:**
- **Cross-plan collision with `agent-audit-remediation` Slice 1** — that plan (PR #868) edits `test-author` + `coderabbit-triage`. → **sequence after PR #868 merges**; rebase on latest `develop` before starting.
- **Extraction loses judgement** — moving too much into a skill could strip the agent's decision guidance. → extract only deterministic mechanics (checklists, skeletons, templates); keep all "when / which / why" in the agent.
- **Skill-parity drift** — the new `test-authoring` skill needs a `version:` + sync header or `test-skill-vs-agent-parity.sh` fails. → covered in Files-to-modify (file 4).
- **Version↔banner drift** — a `version` bump without the banner update fails contract check 10. → bump both in lockstep.

**Non-goals:**
- Shrinking agents already under cap — only the three over-cap ones.
- Changing agent *behaviour* — pure relocation; the routing + invariants stay identical.
- Touching the over-cap baseline for `AGENTS.md` itself — separate concern.

## Verification

- **Bucket A / E**: N/A — no C++.
- **Size**: `agent_size_audit.py --diff origin/develop` shows all three agents under 250 (target ~150); the three baseline entries removed.
- **Contract**: `test-agent-contract.sh` green incl. the new literal-`## Self-improvement` check (and the existing 13); version↔banner lockstep holds for any bumped agent.
- **Parity**: `test-skill-vs-agent-parity.sh` green (the new `test-authoring` skill in `SKILL_ONLY_HELPERS` with a `version:` field).
- **No behaviour drift**: each extracted skill is referenced by its agent with a one-line pointer (manual diff review confirms the agent still names every step, now via the skill).
- **Build gate**: N/A — pure-docs/agentic-shell (`is-pure-docs-diff.sh`).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test against the domain model (sharpen the agent/skill seam wording) before finalising; record the outcome.
- **Manual residue**: none designed. If any verification ends up manual, add a `docs/self-improvement/categories/tooling.md` entry.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise / delete them. In particular, retire the three `agent-size-baseline.md` entries (file 8) so the baseline doesn't claim a cap-exceedance that no longer exists.

- **Shrinking `coderabbit-triage` below the 150 soft-warn** — under 200 is the target; the soft-warn is advisory, deeper shrink is optional polish.
- **A generic "extract to skill" tool** — the extraction is per-agent judgement, not yet mechanizable.

## Implementation log
*(populated post-ship — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
