# Plan — AGENTS.md size reduction via topical extraction

> **Slug**: `agents-md-reduction`
>
> **ADR**: not required — this is a docs reorg that follows the existing `docs/agent-rules/DELEGATION.md` extraction precedent (the original ~230-line lift recorded in `AGENTS.md` § Delegation). No hard-to-reverse semantic change; same rules, different file paths. If a future extraction proves the cross-link cost outweighs the readability win, the affected slice can revert in one PR.

## Context

`AGENTS.md` is 469 lines today (up from ~320 after the original DELEGATION.md split, and ~549 before that). The doc is the **canonical entry point for any agentic harness** (Claude Code, Codex, Cursor, Aider, generic — per the [agents.md spec](https://agents.md/)). Every session reads it at startup. As a result, every line in `AGENTS.md` costs context budget × every-session × every-agent.

The growth since the last reorg is concentrated in **5 sections** that together carry **74% of the doc** (348 of 469 lines):

| Section | Lines | % of file |
|---|---|---|
| `## Project rules` | 118 | 25% |
| `## Merge gates` | 83 | 18% |
| `## Autonomous ship-loop default` | 76 | 16% |
| `## UX Pillars` | 68 | 14% |
| (other 11 sections combined) | 124 | 27% |

The recent merges that drove the growth — the merge-gates CI/CR/user-comments contract, the P4-gated ship-loop subsection, the force-push carve-out extension, the trivial-visual-only envelope, the pure-docs slice skip, the stale-read-on-Edit recipe — are each individually useful rules. They all belong in the project's canonical rule-book. They just don't all belong **inline in the entry-point doc**.

The DELEGATION.md split (which removed ~230 lines from AGENTS.md per the doc's own note) established the pattern: extract a topical rule family to `docs/agent-rules/<topic>.md`, leave a quick-index stub in AGENTS.md (5-10 lines) that names what moved and why, cross-link. Agents that need the detail follow the link; agents that don't never pay the context cost.

After this reorg, **target AGENTS.md size: ≤ 200 lines** (~57% reduction from 469). Same rules, indexed not inlined.

## Approach

**Single-PR topical extraction**, mirroring the existing DELEGATION.md precedent — which was itself a single big-bang ~230-line lift, not a phased reorg. All five topical extractions ship together in one PR, gated by `test-doc-anchors.sh` + `test-agent-contract.sh` + `is-pure-docs-diff.sh`.

**Why one PR, not five**: the extractions are mechanical (copy text, leave stub, update cross-refs); they touch disjoint sections of AGENTS.md (no in-PR ordering required); the merge gates run identically on a 5-extraction diff as on a 1-extraction diff; and reviewer attention is better spent on one cohesive change than amortised across five trivial follow-ups. The earlier 5-phase shape in an earlier draft of this plan was reflexive caution dressed up as risk management — examined under the actual constraints, the slice count had no leverage.

**Sequencing constraint (the one real ordering)**: PR #415 (`feat(p4-gated-ship-loop)`) must merge to `develop` first. Two of the five extractions (Git/p4 discipline, Ship-loops) touch sections #415 added (Force-push carve-out extension wording, P4-gated ship-loop subsection). Lifting those before #415 lands would conflict on those exact lines. Wait for #415 → then one PR for the whole reorg.

**Mechanics** (executed once, covering all five extractions):

1. Create the 5 `docs/agent-rules/<topic>.md` files. Each gets a `# <Topic>` H1, a one-paragraph context preamble cross-linking back to `AGENTS.md`, then the lifted section content verbatim.
2. Replace each original `AGENTS.md` section with a stub: H2/H3 anchor preserved (so existing `§ <name>` cross-references still resolve per `test-doc-anchors.sh`), one-paragraph summary, cross-link to the new file. Stubs are 5-10 lines max.
3. Search-and-replace inline cross-references across `agents/*.md` and other `docs/**.md` files that point at the moved sections, updating them to the new canonical location (the stub stays as a redirect for orphans).
4. Run `bash scripts/dev/test-doc-anchors.sh` — must PASS with zero broken anchors.
5. Run `bash scripts/dev/test-agent-contract.sh` — must PASS (no agent file lost a cross-reference).
6. Run `bash scripts/dev/is-pure-docs-diff.sh develop` — must PASS (exit 0; qualifies for the build+test-all skip).
7. Open PR (draft → ready) — CI's `paths-ignore` skip for docs-only diffs covers the build job; doc-anchors + agent-contract gate the change; CodeRabbit reviews the prose.

**Anchor preservation contract**: every existing `AGENTS.md § <heading>` reference must continue to resolve. The stub in AGENTS.md keeps the heading text exactly as-is; the new file gets an `# <same heading text>` H1 OR an explicit `## <same heading text>` so the doc-anchors checker (which scans both `AGENTS.md` + `docs/agent-rules/*.md`) finds the anchor in either location.

**Extractions (5 sections in one PR)** — "Lines saved" is the inline content removed minus the ~10-line stub left behind:

| # | Section | Target file | Lines saved | Risk |
|---|---|---|---|---|
| 1 | Merge gates → `MERGE_GATES.md` | `docs/agent-rules/MERGE_GATES.md` | ~70 | Low — section is self-contained; 6 explicit consumers (orchestrator, `git-janitor`, `smatchet-merge-watcher`, `merge-gates.sh`, `merge-gates.graphql`, the bats harness) follow cross-links already |
| 2 | UX Pillars → `UX_PILLARS.md` | `docs/agent-rules/UX_PILLARS.md` | ~55 | Low — referenced by plan template + every plan doc's § UX Pillar callouts; replace inline pillar text with a 4-row index table |
| 3 | Project rules — Plan-doc family → `PLAN_RULES.md` | `docs/agent-rules/PLAN_RULES.md` | ~35 | Low — 6 closely-coupled rules (Plan location, Plan-doc safety, Plan revision, Plan stress-test, Plan template, Plan-doc perf-gate). All call sites are inside other plan docs; mechanical S&R |
| 4 | Project rules — Git/p4 discipline family → `GIT_DISCIPLINE.md` | `docs/agent-rules/GIT_DISCIPLINE.md` | ~30 | Medium — touches the force-push carve-out (security-relevant). Stub in AGENTS.md keeps the carve-out's one-line "what's allowed when" summary; detail moves out |
| 5 | Autonomous ship-loop default → `SHIP_LOOPS.md` | `docs/agent-rules/SHIP_LOOPS.md` | ~65 | Medium — the most-referenced section in AGENTS.md (every "default ship-loop" + "P4-gated" + "post-ship turn-end" cross-link). Stub must remain self-contained enough that an agent reading it without following the link still knows: (a) default loop sequence, (b) when to pause, (c) post-ship 4-option menu |

**Total projected reduction in this PR**: ~255 lines lifted, ~50 lines of stubs left behind → net **~205 lines saved**. Final AGENTS.md ≈ 264 lines. (Stretch follow-up: a second PR could also lift `## Project rules` cadence/verification family into `docs/agent-rules/CADENCE.md` to take the final size to ~200 lines, but is explicitly out of scope here — see § Out of scope.)

**What stays inline in AGENTS.md** (deliberately not extracted):

- `## UX Pillars` table-of-pillars (4 rows + agent ownership) — the at-a-glance index.
- `## Autonomous ship-loop default` one-paragraph rule + exceptions list — agent must know this without following a link.
- `## Project rules` items that are 1-liners with no useful detail to move (Build / Language / Layout / Logging / nlohmann json — single-line each).
- `## Debug techniques` (6 lines).
- `## Semantic codebase search` + `§ Semantic-search exceptions` (16 lines combined).
- `## Agent file locations`, `## Delegation` (already an index), `## Self-improvement loop`, `## Dual-VCS topology`, `## Harness adapter`, `## Recommended companion`, `## vexp — Claude-Code-only`.

## Files to modify

**New files** (5):

1. `docs/agent-rules/MERGE_GATES.md` — lifted from `AGENTS.md:149-230`.
2. `docs/agent-rules/UX_PILLARS.md` — lifted from `AGENTS.md:5-72`.
3. `docs/agent-rules/PLAN_RULES.md` — lifted from `AGENTS.md:258-300` (Plan location, Plan-doc safety, Plan revision, Plan stress-test, Plan template, Plan-doc perf-gate).
4. `docs/agent-rules/GIT_DISCIPLINE.md` — lifted from `AGENTS.md:262-283` (Destructive git ops, Destructive p4 ops, Force-push carve-out).
5. `docs/agent-rules/SHIP_LOOPS.md` — lifted from `AGENTS.md:73-148` (Autonomous ship-loop default, P4-gated ship-loop, Post-ship turn-end protocol).

**Modified files** (all in the single PR):

- `AGENTS.md` — replace each of the 5 lifted sections with a 5-10 line stub + cross-link to its new home.
- `agents/*.md` and `docs/**.md` files that reference moved sections by `§ <heading>` — text-search for each lifted heading title; update to point at new location (stub remains as a redirect for orphans). Files known to be touched:
  - Merge-gates referrers: `agents/git-janitor.md` (+ any agent referencing `MERGE_GATES_*` env vars).
  - UX-Pillars referrers: `agents/perf-detective.md`, `agents/spike-hunter.md`, `agents/code-review.md`, `agents/debug-detective.md`, `agents/build-doctor.md` (the 5 pillar-owning agents).
  - Plan-doc family referrers: `docs/design/_plan-template.md` + every existing plan-doc that mentions plan-rules.
  - Git/p4 discipline referrers: `agents/git-janitor.md`, `agents/p4-janitor.md`, `docs/perforce/AGENT_FLOWS.md`.
  - Ship-loops referrers: `agents/git-janitor.md`, `docs/perforce/AGENT_FLOWS.md`, `docs/agent-rules/DELEGATION.md` (Debug-mode pause-loop section already cross-references).

**Anchor compatibility surface**:

- `scripts/dev/test-doc-anchors.sh` — already scans both `AGENTS.md` + `docs/agent-rules/*.md`; no change needed. The script's "Fix options" list already documents the "Update AGENTS.md redirect stub to mention the moved name" recipe (option 3).
- `scripts/dev/test-agent-contract.sh` — verifies agent files declare the required sections; no anchor-level surface; no change needed.

## Existing utilities reused

- `bash scripts/dev/test-doc-anchors.sh` — already in place. Run before push verifies zero broken anchors across the whole reorg.
- `bash scripts/dev/test-agent-contract.sh` — already in place. Run before push verifies no agent-file regression.
- `bash scripts/dev/is-pure-docs-diff.sh develop` — qualifies the PR as a pure-docs slice (write set is strictly `AGENTS.md` + `docs/**`), enabling the build+test-all skip per `AGENTS.md § Pure-docs slice skip`. The PR's CI run gets the same `paths-ignore` skip currently applied to other docs-only PRs.
- `docs/agent-rules/DELEGATION.md` — the original extraction's pattern is the reference for stub shape, cross-link convention, and the "## Delegation" quick-index style.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: N/A — docs only, no runtime path.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: N/A — no UI-thread code touched.
- **Pillar 3 (never crash)**: N/A — no C++ runtime code touched.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: N/A — no UI or visual change.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — diff is strictly `AGENTS.md` + `docs/**`. No `Source_Core/` touch in any phase.

1-5. **All N/A** — no scanner / dispatcher / bucket-E / marker touches.

## Risks / non-goals

**Risks:**

- **Broken anchors at merge time** — an extraction that lifts a section without updating every referrer drops `test-doc-anchors.sh` to FAIL. *Mitigation*: PR gate runs the doc-anchors checker; author must also run it pre-push. Same gate fires once for a 5-extraction diff as it would for a 1-extraction diff.
- **Stub drift** — a future edit to the canonical text in `docs/agent-rules/<topic>.md` doesn't update the corresponding stub in AGENTS.md, producing stale guidance for agents that read only AGENTS.md. *Mitigation*: stubs are deliberately short (5-10 lines) and contain ONLY the rule's one-sentence essence — not enough to drift from canonical. Detail lives in one place by construction.
- **Agent context regression** — an agent that previously read the full rule inline now sees only the stub and has to follow a cross-link. If the agent's harness doesn't follow links, behavior degrades. *Mitigation*: stubs include the rule's name + a one-sentence summary + the cross-link; agents that don't follow links still see the WHAT, just not the WHY/HOW. Compare with the DELEGATION.md split — no measurable degradation observed in the 6+ months since that extraction.
- **Large CR review diff** — one PR carrying ~255 lines lifted + ~50 lines of stubs + cross-reference updates across 8-10 agent files is bigger than CR's per-PR sweet spot. *Mitigation*: the diff is structurally simple (text moves, not logic changes) so CR's noise is bounded; if CR flags too many cosmetic items, address inline rather than re-slicing. Compare with the DELEGATION.md PR which carried a similar lift size without CR issues.
- **Merge conflict with PR #415** — if this reorg's PR opens before #415 merges, both touch the Force-push carve-out section + the P4-gated ship-loop subsection. *Mitigation*: explicit sequencing — wait for #415 to merge before opening the reorg PR. Encoded in § Dependencies.
- **`grill-with-docs` skill might recommend a different topology** — the skill grills plans against the project's glossary + ADRs. For a docs-reorg the glossary impact is null (no new terms) but the skill might surface a smarter grouping (e.g. merge two of the five new files). *Mitigation*: run `grill-with-docs` before sealing this plan; treat it as advisory rather than blocking.

**Non-goals:**

- **Rewriting rule content** — every extracted rule is lifted **verbatim**. Wording changes belong in separate PRs with their own review.
- **Adding new rules** — if the extraction surfaces a missing rule (gap in coverage), file it in `docs/backlog/agent-self-improvement/process.md` rather than fixing inline.
- **Reorganizing the directory structure of `docs/agent-rules/`** — flat layout (matches `DELEGATION.md` precedent). No subdirectories.
- **Touching CLAUDE.md** — `.claude/CLAUDE.md` already just imports `AGENTS.md` via `@../AGENTS.md`. The import auto-picks up the reduction.
- **Renaming the existing `DELEGATION.md`** — keep the all-caps convention even though `MERGE_GATES.md` / `UX_PILLARS.md` / `PLAN_RULES.md` / etc. would arguably read better in kebab-case. Consistency with existing extraction wins; renaming would invalidate every existing cross-link in `agents/*.md` for zero semantic gain.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets per phase:

- **Bucket A (pure-logic ctest)**: N/A — no C++ in any phase.
- **Bucket E (ImGui Test Engine)**: N/A — no UI in any phase.
- **Bash-driver / doc validation**:
  - `bash scripts/dev/test-doc-anchors.sh` — must PASS (0 broken anchors).
  - `bash scripts/dev/test-agent-contract.sh` — must PASS (19/19 sub-checks).
  - `bash scripts/dev/is-pure-docs-diff.sh develop` — must PASS (qualifies for the build+test-all skip).
- **Build gate**: N/A — pure-docs slice per `is-pure-docs-diff.sh`. CI's `paths-ignore` skips the build job.
- **Manual residue**: read each lifted section + its stub end-to-end before opening the PR; confirm the stub captures the rule's WHAT in one sentence. No silent residue.

**Merge-readiness checklist** (also runs in CI):

```bash
wc -l AGENTS.md                          # report reduction (target ~264 lines)
bash scripts/dev/test-doc-anchors.sh    # MUST PASS
bash scripts/dev/test-agent-contract.sh  # MUST PASS
bash scripts/dev/is-pure-docs-diff.sh develop  # MUST PASS (exit 0)
```

## Out of scope (flagged, not designed)

- **A follow-up PR lifting `## Project rules` cadence/verification family** (Build/ctest cadence, Perf slice-boundary auto-run, Pure-docs slice skip, Trivial-visual-only envelope, Stale-read recovery on Edit, Schema-version bumps, Golden-image approval, Verification automation). These are ~50 lines of closely-coupled cadence rules that COULD form a `docs/agent-rules/CADENCE.md` second PR. Deferred because: (a) this PR already delivers the ~205-line reduction, (b) cadence rules are read by every agent on every PR (more sensitive to the stub-drift risk than topic-scoped rules), (c) wait until this PR ships and stub-drift behavior is observed empirically before committing to a second lift.
- **An ADR documenting the topical-extraction convention** — the precedent is already set by the DELEGATION.md extraction. Codifying it as an ADR would be useful but adds review burden; defer.
- **`docs/CONTEXT.md` glossary updates** — no new terms; existing terms unchanged. If `grill-with-docs` surfaces a glossary gap, address inline; otherwise skip.
- **Renaming `docs/agent-rules/DELEGATION.md` to match a new convention** — keep as-is.
- **Automated stub-drift detection** — a check that compares the stub's one-sentence summary in AGENTS.md against the H1 of the corresponding `docs/agent-rules/<topic>.md`. Would catch drift mechanically. Tooling-backlog candidate (`docs/backlog/agent-self-improvement/tooling.md`).
- **Updating other harness-adapter docs** (`.codex/`, `.cursor/` if they ever ship) — those are gitignored and regenerated by `bash scripts/setup-harness.sh <name>` from the canonical `agents/` tree, so they auto-pick-up via re-run.

## Dependencies (sequencing)

- **PR #415** (`feat(p4-gated-ship-loop)`) **must merge to `develop` before this PR opens**. Two of the five extractions (Git/p4 discipline, Ship-loops) touch sections #415 added — opening the reorg PR while #415 is still in CR-review state would conflict on those exact lines. Wait for #415 to merge → then open the reorg PR.
- **`grill-with-docs` skill** — per `AGENTS.md § Plan stress-test`, advisable to run before sealing this plan. The skill grills against `docs/CONTEXT.md` + `docs/adr/`; for a docs-reorg the impact is likely null but worth confirming. Treat as advisory, not blocking.
- **No code dependencies** — no script changes, no `Source_Core/` changes, no Lua binding changes.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
