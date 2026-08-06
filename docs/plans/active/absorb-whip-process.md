<!-- index-summary: absorb the Whip-Process work-item loop (spec → design → plan → impl → close, multi-model review panels, ledgers, linter) into Smatchet's native agent system — no submodule, PowerShell tooling ported to bash/python, panel wired into the verifier-scored review gate. -->
# Plan — Absorb Whip-Process into the Smatchet agent system

> **Slug**: `absorb-whip-process`
>
> **Status**: `active` — blocked-on precondition: `docs/plans/verifier-scored-code-review-gate.md` implemented + archived (Phase 4 extends its ack-verdict machinery; Phases 1–3 could start earlier but landing them against a moving ack schema would churn).

## Context

Whip-Process (`C:\Dev\AndrewsProcess`, standalone repo) is a human-gated work-item loop: spec → design → plan → implementation → close, with a multi-model review panel (~12 legs across claude / codex / cursor / opencode harnesses) at each gate, addresser discipline (independent non-consolidating reviewers, one addresser owning dedup + binding dispositions), tracked ledgers, and a workflow linter. Smatchet's agent system is the opposite pole: autonomous, machine-gated, bash/python-only. The two are complementary — Whip supplies structured *human* gates and genuinely independent multi-model review; Smatchet supplies the enforcement substrate (hooks, merge gates, review-ack, the verifier sidecar).

The decision (this session, superseding an earlier submodule-adoption draft): absorb Whip-Process **fully** into Smatchet's native agent system — its docs become agent-rules docs, its procedures become skills, its PowerShell tooling is ported to bash/python in the portable layer — and retire the standalone repo. No submodule, no consumer contract, no PowerShell.

After this lands: a Smatchet agent can run a full human-gated work item (spec through close) with a multi-model review panel at each gate, and the panel's non-Claude verdicts feed the verifier-scored review gate as an independent scoring source.

## Approach

Port by *role*, not by file. Whip's docs split into two agent-rules docs (the loop; panel mechanics), its per-gate procedures become skills (or fold into existing skills where Smatchet already has the analog), and its three tools are rewritten in the portable layer with their contract semantics preserved. Work items live in a new `docs/work/items/NN-slug/` tree — deliberately **not** under `docs/plans/` — so the existing plan gates (naming, index, ref-integrity, archival nudge) stay untouched and a parallel work-item linter owns the new tree. Boundary rule: product features and multi-gate product work → work items; agent-system / tooling / process changes → plan docs (this tree).

The one genuinely new capability is Phase 4: the panel doubles as the verifier's independent input. `docs/plans/verifier-scored-code-review-gate.md` requires "the verifier must not be the reviewer" — an independent backend, else presence-only fallback. A panel's non-Claude legs (codex, cursor, opencode) are independent of the authoring agent *by construction*, so their structured verdicts can feed `scripts/dev/verifier-produce.py` → `verifier-sidecar.py` aggregate → `review-ack --verdict` with zero new scoring machinery. The panel stays advisory to the addresser for content; only the aggregate score/veto enters the gate.

Human sign-off at the spec and design gates conflicts with the ship-loop's do-not-pause rule; resolved by a scoped exception (ship-loops exception 7): inside a work item, the loop pauses **only** at the two sign-off gates, and everything between them runs autonomously.

Delivery is five phases, each an independently green PR slice:

1. **Docs** — `docs/agent-rules/work-items.md` (the loop, gates, addresser discipline, cascade-vs-absorb change handling, close-collapse), `docs/agent-rules/review-panels.md` (roster, leg independence, output naming `{4|5}-review-[round]-[harness]-[model].md`, guard contract), `docs/work/` scaffold (`items/`, `closed/`, `IDEAS.md`, `BACKLOG.md`, `DEFERRED.md` — product bugs stay in GitHub Issues per ADR-0014, no `Bugs.md`), process-rules + ship-loops updates, one AGENTS.md nav line.
2. **Tooling** — `run-review.sh` (headless legs default; `--panes` opt-in for observable WezTerm grid; `--legs` refill; `--round` required), `review-guard.sh`, `work_item_lint.py`, roster block in `project.config.json`, bats + pytest coverage.
3. **Skills + nudge** — `pre-implementation-review`, `address-review-feedback`, `close-work-item` skills; fold Whip's post-implementation-review deltas into `adversarial-code-review` and its audit deltas into `historical-code-review`; `work-item-owed.sh` SessionStart nudge (pattern: `plan-archival-owed.sh`).
4. **Verifier integration** — panel verdict files parsed into `verifier-produce.py` input, aggregate recorded via the ack-verdict path; `hard_veto` from a panel aggregate blocks at pre-commit exactly like a sidecar veto.
5. **Pilot + retirement** — run one real product work item end-to-end; fix friction; stamp the Whip repo README "absorbed into Smatchet @ `<sha>`" and freeze it.

Port-contract notes that must survive the PS→bash rewrite (the two bugs Whip's tools exist to prevent):

- `review-guard.sh`: dirty-state keyed on **content signature** (status letters + file hash + index blob), not path membership — a leg editing an already-dirty file must not read clean. And git failure returns *unverified*, never "clean" — `null ≠ empty set`; the guard must not fail open.
- `work_item_lint.py`: mechanical subset of the closing review only (file inventory, naming grammar, hierarchical QA-sheet grammar, ledger-citation hygiene — docs cite existing ids, source cites none, retired ids use the `DEF-010 (retired)` form); judgment stays with the closing-review skill.

## Files to modify

Phase 1 — docs:

1. `docs/agent-rules/work-items.md` — new; the work-item loop (port of Whip `Process.md` core).
2. `docs/agent-rules/review-panels.md` — new; panel mechanics (port of Whip `Procedures/ReviewBasics.md`).
3. `docs/work/items/.gitkeep`, `docs/work/closed/.gitkeep`, `docs/work/IDEAS.md`, `docs/work/BACKLOG.md`, `docs/work/DEFERRED.md` — new; work-item tree + ledgers (distinct from the `docs/backlog/` tombstone, which points at plans/self-improvement).
4. `docs/agent-rules/process-rules.md` — add the work-item-vs-plan boundary rule + work-item location/safety rules.
5. `docs/agent-rules/ship-loops.md` — add exception 7 (pause at work-item sign-off gates only).
6. `AGENTS.md` — one nav line for work items (stay under the 150-line cap).

Phase 2 — tooling:

7. `agents/scripts/core/run-review.sh` — new; panel launcher (port of `Tools/run-review.ps1`), headless default.
8. `agents/scripts/core/lib/review-guard.sh` — new; write-guard functions (port of `Tools/review-guard.ps1`, contract above).
9. `agents/scripts/core/work_item_lint.py` — new; workflow linter (port of `Tools/check-docs.ps1` `-Item`/`-Citations` modes).
10. `project.config.json` — roster block (legs, harness commands, model ids) so the portable layer carries no project-specific values.
11. `agents/scripts/core/test-run-review-bats.sh`, `agents/scripts/core/test-review-guard-bats.sh` — new; port Whip's self-test semantics (including the dirty-at-launch guard fixture) with stubbed leg CLIs.
12. `scripts/dev/test_work_item_lint.py` — new; pytest over a ported `qa-mini-item` fixture.
13. `scripts/dev/test-docs.sh` — wire `work_item_lint.py` into the doc-validation suite.

Phase 3 — skills + nudge:

14. `agents/_shared/skills/pre-implementation-review/SKILL.md` — new (port of `Procedures/PreImplementationReview.md`).
15. `agents/_shared/skills/address-review-feedback/SKILL.md` — new (port of `Procedures/AddressReviewFeedback.md`; addresser discipline).
16. `agents/_shared/skills/close-work-item/SKILL.md` — new (port of `Procedures/ClosingItem.md` + judgment half of `ClosingReview.md`; calls the linter for the mechanical half).
17. `agents/_shared/skills/adversarial-code-review/SKILL.md` — fold in Whip post-implementation-review deltas (panel-aware review mode).
18. `agents/_shared/skills/historical-code-review/SKILL.md` — fold in Whip audit deltas (audit drain into `docs/work/`).
19. `agents/scripts/core/work-item-owed.sh` — new; SessionStart nudge for stalled open items.

Phase 4 — verifier integration (paths per `docs/plans/verifier-scored-code-review-gate.md`; exact seams follow whatever that plan ships):

20. `scripts/dev/verifier-produce.py` — accept panel verdict files as an input source.
21. `agents/scripts/core/review-ack.sh` / `agents/scripts/core/lib/review-ack.sh` — record panel-aggregate verdicts through the same ack-verdict path.
22. `agents/scripts/core/test-review-ack-gate-bats.sh` — panel-verdict cases.

Phase 5 — pilot + retirement:

23. Whip repo `README.md` (external, `C:\Dev\AndrewsProcess`) — absorption stamp; not a Smatchet path.

## Existing utilities reused

- `review-ack.sh` + `lib/review-ack.sh` (`agents/scripts/core/`) — the ack fingerprint + verdict machinery; Phase 4 extends it, never duplicates it.
- `scripts/dev/verifier-produce.py`, `scripts/dev/verifier-sidecar.py` — the scoring pipeline; panel verdicts become one more input, no parallel scorer.
- `scripts/dev/pre-ship.sh` — existing scoring call site; panel aggregation hooks in there, not a new hook.
- `agents/scripts/core/plan-archival-owed.sh` — the SessionStart-nudge pattern `work-item-owed.sh` copies.
- `scripts/dev/test-docs.sh` — the doc-gate umbrella the work-item linter joins.
- `agents/_shared/skills/adversarial-code-review/`, `agents/_shared/skills/historical-code-review/` — existing skills that absorb Whip's post-impl-review and audit deltas instead of new sibling skills.
- `project.config.json` — the portable-purity value store; the roster lives there.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — this plan imports content from an external repo; it does not split any over-cap Smatchet file. The only cap in play is `AGENTS.md` ≤ 150, held to one added nav line.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — docs, skills, and dev-time tooling only; no product code.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — no UI-thread code touched.
- **Pillar 3 (never crash)**: no impact — no product code; tooling failures surface as gate failures, not runtime behavior.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no impact — no UI changes.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — no `Source_Core/` (or any `Source/`) changes in any phase; docs + agent tooling only.

## Risks / non-goals

Risks:

- **PS→bash semantic drift in the guard port** — the fail-open and false-clean bugs are exactly what the original prevents. Mitigation: port the contract comments verbatim and replicate Whip's self-test fixtures (dirty-at-launch edit, git-failure → unverified) in bats.
- **Panel legs need live, authenticated model CLIs** — CI can't run them. Mitigation: headless default with roster degradation (missing CLI → leg skipped + reported), bats coverage via stubbed CLIs, live smoke stays operator-run (see § Verification, manual residue).
- **Human gates vs autonomous loop** — a broadly-worded exception erodes do-not-pause. Mitigation: exception 7 is scoped to exactly the two sign-off gates inside an open work item, nothing else.
- **New doc tree escapes existing gates** — `docs/work/` is covered by neither plan gates nor md-link defaults until wired. Mitigation: Phase 2 wires `work_item_lint.py` into `test-docs.sh` before Phase 5 produces real items; Phase 1 tree ships only scaffold + ledgers.
- **Verifier-plan drift** — Phase 4 depends on seams that plan hasn't shipped yet. Accepted: the Status header blocks Phase 4 on it; Phases 1–3 don't touch the ack schema.

Non-goals:

- No submodule mount, no `install.ps1`/`Harness/` stamping, no consumer contract — Whip stops being a distributable.
- No PowerShell retained — ports only, per the PS-retirement line (PRs #1955–#1960).
- No WezTerm-pane default — observable panes are `--panes` opt-in; Smatchet's default is headless.
- No GenericReview port — CodeRabbit + Bugbot already cover ad-hoc review.
- No migration of Whip's own internal `Documentation/Work/` history — the repo is frozen, not imported.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no C++ changes.
- **Bucket E (ImGui Test Engine)**: N/A — no UI changes.
- **Bash-driver scenario / screenshot / sanitizer**: bats suites for `run-review.sh` (stubbed leg CLIs; roster degradation; `--legs` refill) and `review-guard.sh` (already-dirty-file edit detected; git failure reports unverified, not clean); pytest for `work_item_lint.py` over the ported `qa-mini-item` fixture.
- **Build gate**: N/A — no `Source/` changes; dual-target build untouched.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome. Outcome: pending — to be run and recorded here before Phase 1 implementation starts.
- **Manual residue**: the live panel smoke (real model CLIs, real auth) is operator-run by design — CI can't hold the credentials. Deferred-automation action: none planned (accepted residue); add a `docs/self-improvement/categories/tooling.md` entry recording the residue in the Phase 2 PR.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **Retro mechanics** — Whip's retro step maps naturally onto the self-improvement system; folding it in is a follow-up once real work items exist to retro.
- **QA.md standing checklist** — Smatchet's verification is automated; a standing manual-QA sheet gets designed only if the pilot item surfaces a manual surface.
- **Cross-worktree panel runs** — panels run in the item's own worktree; orchestrating a panel across the ~7–9 parallel worktrees is not designed here.
- **Migrating existing active plans into work items** — the boundary rule applies to new work only; nothing is reclassified.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.** Writing the actual basename here
   > manufactures a `docs/plans/shipped/<name>.md` path that points at a file
   > still living in `active/` (the move hasn't happened yet), which
   > `test-plan-ref-integrity.sh` reports as a dangling self-reference. The gate
   > carves out the *placeholder* form on the Archive `git mv` line; the
   > expanded form defeats that carve-out. Run the literal command with your
   > slug substituted at the shell — never bake the expansion into the file.
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
