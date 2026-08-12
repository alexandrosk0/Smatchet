# Plan — AGENTS.md size reduction via topical extraction
<!-- plan-date: 2026-05-23 -->

> **Slug**: `agents-md-reduction`
>
> **ADR**: not required — this is a docs reorg that follows the existing `docs/agent-rules/DELEGATION.md` extraction precedent (the original ~230-line lift recorded in `AGENTS.md` § Delegation). No hard-to-reverse semantic change; same rules, different file paths. If a future extraction proves the cross-link cost outweighs the readability win, the affected slice can revert in one PR.
>
> **`grill-with-docs` pass**: completed against this plan-doc on 2026-05-23 — 7 decisions crystallised (D1 kebab-case naming + rename `DELEGATION.md` → `delegation.md`; D2 merge `plan-rules` + `git-discipline` → single `process-rules.md`; D3 compress § Dual-VCS topology to stub pointing at `AGENT_FLOWS.md`; D4 fold cadence/verification family into `process-rules.md`; D5 canonicalise new cross-refs on form B; D6 load-bearing stubs Option Y; D7 add "where do new rules go?" meta-rule to `process-rules.md`). All seven applied in `refactor(plan): apply grill-with-docs outcomes` commit. ADR criteria still don't fire (3-of-3 needed; only the "real trade-off" criterion fires unambiguously).

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

Plus § Dual-VCS topology (23 lines) — itself mostly a stub-shaped section that already cross-links to `docs/perforce/AGENT_FLOWS.md` for everything; absorbed into the existing AGENT_FLOWS.md in this PR per grill D3 (no new file needed).

The recent merges that drove the growth — the merge-gates CI/CR/user-comments contract, the P4-gated ship-loop subsection, the force-push carve-out extension, the trivial-visual-only envelope, the pure-docs slice skip, the stale-read-on-Edit recipe — are each individually useful rules. They all belong in the project's canonical rule-book. They just don't all belong **inline in the entry-point doc**.

The DELEGATION.md split (which removed ~230 lines from AGENTS.md per the doc's own note) established the pattern: extract a topical rule family to `docs/agent-rules/<topic>.md`, leave a quick-index stub in AGENTS.md (5-10 lines) that names what moved and why, cross-link. Agents that need the detail follow the link; agents that don't never pay the context cost. Note: the original `DELEGATION.md` was uppercase; this PR renames it to `delegation.md` (kebab-case) per grill D1, matching the dominant Smatchet convention + the already-kebab `golden-image-approval.md` sibling.

After this reorg, **target AGENTS.md size: ≤ 200 lines** (~59% reduction from 469). Same rules, indexed not inlined.

## Approach

**Single-PR topical extraction**, mirroring the existing DELEGATION.md precedent — which was itself a single big-bang ~230-line lift, not a phased reorg. All five topical extractions ship together in one PR, gated by `test-doc-anchors.sh` + `test-agent-contract.sh` + `is-pure-docs-diff.sh`.

**Why one PR, not five**: the extractions are mechanical (copy text, leave stub, update cross-refs); they touch disjoint sections of AGENTS.md (no in-PR ordering required); the merge gates run identically on a 5-extraction diff as on a 1-extraction diff; and reviewer attention is better spent on one cohesive change than amortised across five trivial follow-ups. The earlier 5-phase shape in an earlier draft of this plan was reflexive caution dressed up as risk management — examined under the actual constraints, the slice count had no leverage.

**Sequencing constraint (the one real ordering)**: PR #415 (`feat(p4-gated-ship-loop)`) must merge to `develop` first. Two of the five extractions (Git/p4 discipline, Ship-loops) touch sections #415 added (Force-push carve-out extension wording, P4-gated ship-loop subsection). Lifting those before #415 lands would conflict on those exact lines. Wait for #415 → then one PR for the whole reorg.

**Mechanics** (executed once, covering all four extractions + the DELEGATION.md rename + the Dual-VCS absorption):

1. Create the 4 new `docs/agent-rules/<topic>.md` files (kebab-case per D1). Each gets a `# <Topic>` H1, a one-paragraph context preamble cross-linking back to `AGENTS.md`, then the lifted section content verbatim.
2. Rename existing `docs/agent-rules/DELEGATION.md` → `docs/agent-rules/delegation.md` per D1 (same content; updates every cross-ref that names the file).
3. Replace each original `AGENTS.md` section with a load-bearing stub per the Stub format rule (D6 / § Stub format) below.
4. Move § Dual-VCS topology's content into the existing `docs/perforce/AGENT_FLOWS.md` (no new file); replace AGENTS.md's § Dual-VCS topology with a 5-line stub pointing there per D3.
5. Search-and-replace cross-references across `agents/*.md` and `docs/**.md` files that point at the moved sections — update to form B (`docs/agent-rules/<file>.md § <subsection>` per D5) for any reference we're touching for another reason; leave form-A `AGENTS.md § <name>` references that don't need other changes alone (no churn-just-for-canonicalisation).
6. Run `bash scripts/dev/test-doc-anchors.sh` — must PASS with zero broken anchors.
7. Run `bash scripts/dev/test-agent-contract.sh` — must PASS (no agent file lost a cross-reference).
8. Run `bash scripts/dev/is-pure-docs-diff.sh develop` — must PASS (exit 0; qualifies for the build+test-all skip).
9. Open PR (draft → ready) — CI's `paths-ignore` skip for docs-only diffs covers the build job; doc-anchors + agent-contract gate the change; CodeRabbit reviews the prose.

**Anchor preservation contract**: every existing `AGENTS.md § <heading>` reference must continue to resolve. The stub in AGENTS.md keeps the heading text exactly as-is; the new file gets an `# <same heading text>` H1 OR an explicit `## <same heading text>` so the doc-anchors checker (which scans both `AGENTS.md` + `docs/agent-rules/*.md`) finds the anchor in either location.

### Stub format (Option Y — load-bearing stubs, D6)

Every stub in AGENTS.md follows this shape:

1. `## <section name>` heading — preserves the `AGENTS.md § <name>` anchor exactly as it was before extraction.
2. One paragraph naming the rule's **WHAT** (the load-bearing summary an agent that doesn't follow links still gets).
3. 1–3 must-know invariants inline (as bullets, a mini-table, or a code block when the rule **is** a sequence). These are the load-bearing facts that change behaviour if a reader misses them.
4. Cross-link line: `Full <enumeration of moved concepts>: [\`docs/agent-rules/<file>.md\`](docs/agent-rules/<file>.md)`.

**Target length**: 5–12 lines per stub. Single rule, applied to every stub. Reviewers reject stubs that are too thin (< 5 lines, missing must-know invariants) **or** too fat (> 12 lines, duplicating canonical content rather than redirecting). Stub-edit reviewers verify: stub's WHAT + invariants match the canonical file's first paragraph.

### Cross-reference convention (D5)

Going forward, NEW cross-references target form B: `` `docs/agent-rules/<file>.md` § <subsection> ``. The stubs in AGENTS.md exist as redirects for legacy form-A `AGENTS.md § <name>` references that nobody is touching for unrelated reasons — they continue to work but are not the form anyone writes from this PR onwards. Existing form-A refs in agent files stay as-is; only refs we touch for other edit reasons get canonicalised on the way past.

### Extractions (4 sections in one PR — post-grill topology)

"Lines saved" is the inline content removed minus the ~10-line stub left behind.

| # | Section(s) | Target file | Lines saved | Risk |
|---|---|---|---|---|
| 1 | Merge gates → `merge-gates.md` | `docs/agent-rules/merge-gates.md` | ~70 | Low — self-contained; consumers (`git-janitor`, `smatchet-merge-watcher`, `merge-gates.sh`, `merge-gates.graphql`, bats harness) follow cross-links already |
| 2 | UX Pillars → `ux-pillars.md` | `docs/agent-rules/ux-pillars.md` | ~55 | Low — every plan template's § UX Pillar callouts will switch from inline-pillar text to a 4-row index table referencing the canonical file |
| 3 | Autonomous ship-loop default → `ship-loops.md` (includes P4-gated + post-ship turn-end) | `docs/agent-rules/ship-loops.md` | ~65 | Medium — most-referenced section. Stub must remain self-contained enough that a non-link-following agent knows (a) default loop sequence, (b) when to pause, (c) post-ship 4-option menu |
| 4 | Process rules (combined): Plan-doc family + Git/p4 discipline + Cadence/verification + Meta-rule "where do new rules go?" → `process-rules.md` | `docs/agent-rules/process-rules.md` | ~115 (single combined file) | Medium — touches the force-push carve-out (security-relevant) and the cadence/verification family that every agent reads on every PR. Stub-drift risk concentrated here; mitigated by D6 load-bearing format |

**Plus three structural moves** (not new files):

- **Rename** `docs/agent-rules/DELEGATION.md` → `docs/agent-rules/delegation.md` per D1.
- **Compress** § Dual-VCS topology in AGENTS.md (23 lines) into a 5-line stub pointing at `docs/perforce/AGENT_FLOWS.md`; absorb the original table into AGENT_FLOWS.md per D3.
- **Meta-rule "where do new rules go?"** lands inside `process-rules.md` per D7 (no new file).

**Total projected reduction in this PR**: ~255 lines lifted from AGENTS.md + ~18 lines from Dual-VCS compression − ~50 lines of stubs left behind = net **~270 lines saved**. Final AGENTS.md ≈ **~199 lines** (target ≤ 200 met).

### Meta-rule "where do future rules go?" (D7)

Lands inside `process-rules.md` (with a stub in AGENTS.md pointing at it). Text:

> **Where to add a new agent-rule:**
> 1. **Build / language / quality 1-liner that every agent must see** (Build, Language, Logging, Lint, etc. shape) → `AGENTS.md § Project rules` inline.
> 2. **A rule that fits within an existing extracted topic** (`merge-gates.md`, `ux-pillars.md`, `ship-loops.md`, `process-rules.md`) → add to that file. Update the corresponding AGENTS.md stub only if the rule changes the load-bearing essence.
> 3. **A new topic that doesn't fit any existing file AND is > 30 lines** → create new `docs/agent-rules/<topic>.md` (kebab-case) + add stub to AGENTS.md.
> 4. **A new topic that doesn't fit AND is ≤ 30 lines** → put it in `process-rules.md` (the catch-all) rather than fragmenting.

**What stays inline in AGENTS.md** (deliberately not extracted):

- `## Project rules` items that are 1-liners with no useful detail to move (Build / Language / Layout / Logging / nlohmann json — single-line each).
- `## Debug techniques` (6 lines).
- `## Semantic codebase search` + `§ Semantic-search exceptions` (16 lines combined).
- `## Agent file locations`, `## Delegation` (already an index pointing at `delegation.md`), `## Self-improvement loop`, `## Harness adapter` (high read-frequency by every agent that declares `capabilities:` — extracting would trade convenience-for-every-agent for ~15 lines saved per D3), `## Recommended companion`, `## vexp — Claude-Code-only`.

## Files to modify

**New files** (4, all kebab-case per D1):

References below name **sections** (and named sub-rules within § Project rules), not numeric line ranges — line numbers shift as AGENTS.md evolves, and the three sub-rule families lifted into `process-rules.md` are **interleaved bold sub-bullets** within a single `## Project rules` section, not contiguous line ranges.

1. `docs/agent-rules/merge-gates.md` — lifted from `AGENTS.md` § Merge gates (CI / CR / user-comment semantics, halt prompts, env knobs, REST-merge contract).
2. `docs/agent-rules/ux-pillars.md` — lifted from `AGENTS.md` § UX Pillars (Pillars 1-4 enforceable invariants + agent ownership table).
3. `docs/agent-rules/ship-loops.md` — lifted from `AGENTS.md` § Autonomous ship-loop default (the section's full body — default sequence, exceptions list, § P4-gated ship-loop subsection, § Post-ship turn-end protocol subsection).
4. `docs/agent-rules/process-rules.md` — combined per D2 + D4 + D7, lifted as named sub-rules from `AGENTS.md` § Project rules:
   - **Plan-doc family**: § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test — `grill-with-docs` skill, § Plan template — start from `docs/plans/active/_plan-template.md`, § Plan-doc perf-gate section.
   - **Git/p4 discipline**: § Destructive git ops in shared worktrees, § Destructive `p4` ops in p4-mode, § Force-push carve-out for Claude Code SDK-spawned recovery and p4 task-stream promotion.
   - **Cadence/verification family**: § Verification automation — zero manual steps, § Schema-version bumps, § Trivial-visual-only change envelope, § Build / ctest cadence — slice-boundary only (with its § P4-gated loops carve-out), § Perf slice-boundary auto-run — scenario-aware, § Pure-docs slice skip, § Stale-read recovery on `Edit`.
   - Meta-rule "where do new rules go?" (D7 — newly authored, not lifted).

Sub-rules NOT extracted from § Project rules (deliberately kept inline per § Approach § "What stays inline in AGENTS.md"): § Build, § Language, § Layout, § Available libs, § Logging, § nlohmann json, § Optional plugins, § Don't, § Dual-target, § Quality, § Lint, § Perf workflow, § Golden-image approval contract. These are 1-line invariants that every agent must see on first read; extracting them would cost more in cross-link follow than it saves in AGENTS.md height.

**Renames** (1):

- `docs/agent-rules/DELEGATION.md` → `docs/agent-rules/delegation.md` per D1. Same content; updates the file's name to match the new kebab-case convention applied to all four new files + the existing `golden-image-approval.md` sibling.

**Modified files** (all in the single PR):

- `AGENTS.md` — replace each of the 4 lifted topic-groups with a 5-12 line load-bearing stub per the Stub format rule (D6) above + cross-link to its new home. Compress § Dual-VCS topology to a 5-line stub pointing at `docs/perforce/AGENT_FLOWS.md` per D3. Update the existing § Delegation index entry to reference `delegation.md` (lowercase) per D1.
- `docs/perforce/AGENT_FLOWS.md` — absorb the table + content from AGENTS.md § Dual-VCS topology per D3 (the table fits naturally alongside the existing TL;DR table).
- `agents/*.md` and `docs/**.md` files that reference moved sections — for files we're touching for any reason (e.g. an existing form-A reference that points at a section now in a different stub), canonicalise on form B per D5 (`docs/agent-rules/<file>.md § <subsection>`). Files known to be touched:
  - Merge-gates referrers: `agents/core/git-janitor.md` (+ any agent referencing `MERGE_GATES_*` env vars).
  - UX-Pillars referrers: `agents/core/perf-detective.md`, `agents/core/spike-hunter.md`, `agents/core/code-review.md`, `agents/core/debug-detective.md`, `agents/core/build-doctor.md` (the 5 pillar-owning agents).
  - Process-rules referrers (combined): `docs/plans/active/_plan-template.md` + every existing plan-doc that mentions plan-rules + `agents/core/git-janitor.md` + `agents/core/p4-janitor.md` + `docs/perforce/AGENT_FLOWS.md`.
  - Ship-loops referrers: `agents/core/git-janitor.md`, `docs/perforce/AGENT_FLOWS.md`, `docs/agent-rules/delegation.md` (Debug-mode pause-loop section already cross-references).
  - DELEGATION.md → delegation.md rename: every file that names the old path (`DELEGATION.md`) gets path lowercased. Run `git grep -nF 'DELEGATION.md'` to enumerate.
- Form-A refs in files NOT being touched for any other reason: leave alone (no churn just for canonicalisation per D5).

**Anchor compatibility surface**:

- `scripts/dev/test-doc-anchors.sh` — already scans both `AGENTS.md` + `docs/agent-rules/*.md`; no change needed. The script's "Fix options" list already documents the "Update AGENTS.md redirect stub to mention the moved name" recipe (option 3).
- `scripts/dev/test-agent-contract.sh` — verifies agent files declare the required sections; no anchor-level surface; no change needed.

## Existing utilities reused

- `bash scripts/dev/test-doc-anchors.sh` — already in place. Run before push verifies zero broken anchors across the whole reorg.
- `bash scripts/dev/test-agent-contract.sh` — already in place. Run before push verifies no agent-file regression.
- `bash scripts/dev/is-pure-docs-diff.sh develop` — qualifies the PR as a pure-docs slice (write set is strictly `AGENTS.md` + `docs/**`), enabling the build+test-all skip per `AGENTS.md § Pure-docs slice skip`. The PR's CI run gets the same `paths-ignore` skip currently applied to other docs-only PRs.
- `docs/agent-rules/DELEGATION.md` → `delegation.md` after rename — the original extraction's pattern is the reference for stub shape, cross-link convention, and the "## Delegation" quick-index style.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: N/A — docs only, no runtime path.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: N/A — no UI-thread code touched.
- **Pillar 3 (never crash)**: N/A — no C++ runtime code touched.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: N/A — no UI or visual change.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — diff is strictly `AGENTS.md` + `docs/**`. No `Source_Core/` touch.

1-5. **All N/A** — no scanner / dispatcher / bucket-E / marker touches.

## Risks / non-goals

**Risks:**

- **Broken anchors at merge time** — an extraction that lifts a section without updating every referrer drops `test-doc-anchors.sh` to FAIL. *Mitigation*: PR gate runs the doc-anchors checker; author must also run it pre-push.
- **Stub drift** — a future edit to the canonical text in `docs/agent-rules/<file>.md` doesn't update the corresponding load-bearing stub in AGENTS.md. *Mitigation*: per D6, stubs target 5-12 lines containing only the rule's WHAT + must-know invariants — short enough that drift is visible on every canonical edit. The stub-drift detection automation (a check that the stub's WHAT + invariants match the canonical's first paragraph) stays as backlog work; if drift becomes a recurring issue in practice it gets promoted.
- **Agent context regression** — an agent that previously read the full rule inline now sees only the stub and has to follow a cross-link. If the agent's harness doesn't follow links, behavior degrades. *Mitigation*: D6 load-bearing stubs include must-know invariants inline; agents that don't follow links still get the load-bearing facts, just not the full derivation. Compare with the existing `delegation.md` split — no measurable degradation observed in the 6+ months since that extraction.
- **Large CR review diff** — one PR carrying ~270 lines lifted + ~50 lines of stubs + cross-reference updates + DELEGATION.md rename + Dual-VCS absorption is bigger than CR's per-PR sweet spot. *Mitigation*: the diff is structurally simple (text moves + path renames, not logic changes) so CR's noise is bounded; if CR flags too many cosmetic items, address inline rather than re-slicing. Compare with the original DELEGATION.md PR which carried a similar lift size without CR issues.
- **Merge conflict with PR #415** — if this reorg's PR opens before #415 merges, both touch the Force-push carve-out section + the P4-gated ship-loop subsection. *Mitigation*: explicit sequencing — wait for #415 to merge before opening the reorg PR. Encoded in § Dependencies.
- **DELEGATION.md → delegation.md rename breaks external references** — any external doc, comment, or third-party tool that hard-codes the old uppercase path stops resolving. *Mitigation*: case-insensitive search via `git grep -nFi 'DELEGATION.md'` catches every in-repo reference; out-of-repo references (third-party tooling, external wikis) are unlikely for this internal doc but flagged here for awareness.

**Non-goals:**

- **Rewriting rule content** — every extracted rule is lifted **verbatim**. Wording changes belong in separate PRs with their own review. The one exception is the D7 meta-rule which is genuinely new content; it lands in `process-rules.md` per D7 + § Meta-rule above.
- **Adding new rules** — if the extraction surfaces a missing rule (gap in coverage), file it in `docs/self-improvement/categories/process.md` rather than fixing inline.
- **Reorganizing the directory structure of `docs/agent-rules/`** — flat layout (matches existing precedent — `delegation.md`, `golden-image-approval.md`, plus the 4 new files). No subdirectories.
- **Touching CLAUDE.md** — `.claude/CLAUDE.md` already just imports `AGENTS.md` via `@../AGENTS.md`. The import auto-picks up the reduction.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest)**: N/A — no C++.
- **Bucket E (ImGui Test Engine)**: N/A — no UI.
- **Bash-driver / doc validation**:
  - `bash scripts/dev/test-doc-anchors.sh` — must PASS (0 broken anchors).
  - `bash scripts/dev/test-agent-contract.sh` — must PASS (19/19 sub-checks).
  - `bash scripts/dev/is-pure-docs-diff.sh develop` — must PASS (qualifies for the build+test-all skip).
- **Build gate**: N/A — pure-docs slice per `is-pure-docs-diff.sh`. CI's `paths-ignore` skips the build job.
- **Manual residue**: read each lifted section + its load-bearing stub end-to-end before opening the PR; confirm the stub satisfies the D6 format rule (5-12 lines, WHAT + must-know invariants + cross-link); confirm form-B cross-refs canonicalisation matches D5. No silent residue.

**Merge-readiness checklist** (also runs in CI):

```bash
wc -l AGENTS.md                          # report reduction (target ≤ 200 lines)
bash scripts/dev/test-doc-anchors.sh    # MUST PASS
bash scripts/dev/test-agent-contract.sh  # MUST PASS
bash scripts/dev/is-pure-docs-diff.sh develop  # MUST PASS (exit 0)
```

## Out of scope (flagged, not designed)

- **An ADR documenting the topical-extraction convention** — the precedent is already set by the existing `delegation.md` extraction. ADR criteria (3-of-3: hard-to-reverse + surprising + real-trade-off) fire only on (3); plan-doc captures the rationale durably. Defer.
- **`docs/CONTEXT.md` glossary updates** — `grill-with-docs` confirmed no new Smatchet-domain terms; "stub", "extracted rule", "topical extraction" are generic doc-org concepts that don't belong in the project glossary.
- **Automated stub-drift detection** — a check that compares the stub's WHAT + invariants in AGENTS.md against the H1 + opening of the corresponding `docs/agent-rules/<file>.md`. Would catch drift mechanically. Tooling-backlog candidate (`docs/self-improvement/categories/tooling.md`); promotion-criterion: drift observed in practice within 3 months of this PR landing.
- **Updating other harness-adapter docs** (`.codex/`, `.cursor/` if they ever ship) — those are gitignored and regenerated by `bash scripts/setup-harness.sh <name>` from the canonical `agents/` tree, so they auto-pick-up via re-run.
- **Bulk-rewriting existing form-A cross-references to form B** — per D5, only canonicalise refs that we're touching for another reason. A standalone "canonicalise all form-A refs" PR is not on the roadmap; it's churn without leverage as long as the doc-anchors checker resolves both forms.

## Dependencies (sequencing)

- **PR #415** (`feat(p4-gated-ship-loop)`) **must merge to `develop` before this PR opens**. Two of the four extractions (`process-rules.md` for Git/p4 discipline + cadence; `ship-loops.md` for P4-gated subsection) touch sections #415 added — opening the reorg PR while #415 is still in CR-review state would conflict on those exact lines. Wait for #415 to merge → then open the reorg PR.
- **`grill-with-docs` skill** — completed 2026-05-23 against this plan-doc. 7 decisions crystallised (D1-D7) and applied via `refactor(plan): apply grill-with-docs outcomes` commit. No ADR fired, no glossary updates needed.
- **No code dependencies** — no script changes, no `Source_Core/` changes, no Lua binding changes.

## Implementation log
- `994068fe` · #416 — `wip(plan)` commit.
- `96ab99fd` · #417 (2026-05-23) — lifted 4 topical sections out of AGENTS.md (−344 lines) into `docs/agent-rules/{merge-gates.md, ux-pillars.md, ship-loops.md, process-rules.md}`, each left with a stub + cross-link in AGENTS.md. Renamed `DELEGATION.md` → `delegation.md` (0 content change) and absorbed the Git/p4 discipline text into `docs/perforce/AGENT_FLOWS.md`.

## Deviations from plan
- **Dependency satisfied as designed** — PR #415 (`feat(p4-gated-ship-loop)`) merged first, so the `process-rules.md` + `ship-loops.md` extractions applied cleanly with no conflict.
- **AGENTS.md has since drifted to ~216 lines** (> the ~200 target) from later, unrelated rule additions. This is not a regression of this plan, which landed AGENTS.md at ~199 lines; the stub-anchored structure is intact.

## Verification (actual)
- **Bucket A:** `test-doc-anchors.sh` + `test-agent-contract.sh` PASS — every cross-link from the 4 new `docs/agent-rules/` files + the AGENTS.md stubs resolves.
- **Pure-docs:** `is-pure-docs-diff.sh` confirmed the diff as docs-only → build + test-all skipped per cadence rule.
- **File presence:** all 4 extracted files exist with the expected stub-and-detail shape.
