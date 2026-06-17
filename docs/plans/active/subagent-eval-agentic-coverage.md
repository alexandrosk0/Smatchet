# Plan — Subagent eval agentic coverage (close the three eval-gap dimensions + judge calibration + trace flywheel)

> **Slug**: `subagent-eval-agentic-coverage` (matches this file's basename without `.md`).
>
> **Status**: `active` — **not started**; only the parent #650 schemas exist on develop (Phases 0–4 of this plan unbuilt). Phased; each phase ships as its own PR (prove-first ordering, § Approach).
>
> **Scope clarifier**: evaluates the **development agents** (`agents/*.md` — the orchestrator + the ~30 delegated subagents), NOT the Smatchet product or any in-app AI-assistant surface. Same scope as the parent `subagent-eval-harness.md` (shipped) and the now-absorbed `subagent-eval-flywheel.md`.
>
> **Absorbs**: `docs/plans/subagent-eval-flywheel.md` — that plan's trace-flywheel content is folded into Phase 2 here; the old file is marked `deferred — superseded by this plan` in the same PR that lands this doc.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The Phase-1 subagent-eval harness shipped (`docs/plans/shipped/subagent-eval-harness.md`, PR #650): three JSON schemas + a pure-stdlib base-vs-head scorer (external judge) + a runner (`--prompt-root` / `--fake-runner`) + bats + 3 real `code-review` golden cases, wired **advisory** (malformed artifact FAILs, quality regression WARNs). It proved the scoring contract against deterministic fixtures.

Measured against the state-of-the-art framing in *"The Art & Science of Benchmarking Agents"* (Vincent Chen, Snorkel AI — <https://www.youtube.com/watch?v=iNkFlCiij0U>; companion: *"Closing the Evaluation Gap in Agentic AI"*, <https://snorkel.ai/blog/closing-the-evaluation-gap-in-agentic-ai/>), the MVP is strong on the **science fundamentals** (reproducible, real-commit-anchored cases, anti-benchmaxxing, calibration-honest WARN-not-BLOCK) but covers **none of the three agentic eval-gap dimensions** the talk names:

1. **Environment complexity** — domain-specific unwritten constraints, noisy context, realistic tools with rate-limits / ambiguous docs, multi-agent coordination.
2. **Autonomy horizon** — reliability over long trajectories (hundreds of steps), recovery from setbacks, non-stationary goals. Smatchet's actual product *is* a long-horizon autonomous ship-loop, yet that trajectory ships **unbenchmarked** — the single biggest gap.
3. **Output complexity** — multi-artifact deliverables, **multi-factor rubrics beyond pass/fail**, risk calibration / uncertainty / escalation judgment.

Two cross-cutting enablers gate progress. **Judge calibration** (judge-vs-human agreement corpus) is the critical path: it unblocks the WARN→BLOCK graduation *and* makes auto-scored flywheel candidates trustworthy — it is the prerequisite the parent plan and the flywheel plan both deferred to (`docs/self-improvement/categories/tooling.md` 2026-05-31 P2). The **trace flywheel** (grow the golden set from real production traces — the talk's flywheel lesson) is already planned but **BLOCKED on calibration**, and its harvested trajectories are exactly what the autonomy-horizon dimension needs as input.

**Intended outcome (one sentence):** after this lands, the dev-agent eval harness has a calibrated judge that can graduate dimensions WARN→BLOCK, scores all three agentic-gap dimensions (output-complexity rubrics, orchestrator ship-loop trajectories, multi-agent + tool-fault environments), and grows its own golden set from redacted real traces — closing the evaluation gap the Snorkel framing names while staying reproducible and human-curated.

## Approach

**Prove-first, dependency-ordered.** The five phases below are each independently shippable and independently valuable; the order is the dependency chain (calibration unblocks everything; the flywheel feeds the trajectory dimension its raw material). The two most infra-heavy dimensions (autonomy-horizon, environment-complexity) come last and are gated behind the cheaper phases demonstrating the pattern earns its keep — the same prove-then-build discipline as `docs/plans/active/harness-audit-suite.md`.

Every phase reuses the shipped contract seams — the external-judge boundary (keeps `agent-eval-score.py` pure-stdlib + deterministic), the `--prompt-root` before/after seam, the `--fake-runner` / fake-judge no-token CI path, and the harness-adapter seam (transcript-shape is Claude-Code-specific, the emitted case/result/scoring formats stay portable). New scored dimensions are **objective wherever possible** (deterministic inline checks, no LLM noise) and fall back to the external judge only for genuinely qualitative axes (goal-adherence over a trajectory).

**Phase order (each = one PR):**

- **Phase 0 — Judge calibration loop** *(critical path; cheapest; retires the P2 residue)*. Stand up `agent-eval-calibrate.py` (pure stdlib): consume a labelled corpus of `(run, dimension, judge_score, human_label)`, compute agreement (Cohen's κ for categorical, Spearman/Pearson + raw % for continuous), emit a calibration report, and propose per-dimension BLOCK thresholds. Add a `block` flag + `calibration` provenance to `scoring-policy.json`; teach `agent-eval-score.py` to treat a regression on a **calibrated, `block:true`** dimension as a hard fail (exit 1 → CI BLOCK) while uncalibrated dimensions stay advisory. Graduate the existing `code-review` objective dimensions first (they have zero judge noise — `cited_file_line` is already `max_score_drop: 0.0`).
- **Phase 1 — Gap-3 output-complexity dimensions** *(cheap; reuses single-shot `code-review` cases)*. Add objective checks `uncertainty_flagged` / `escalation_present` and a multi-factor weighted **judge rubric** (not a single 0..1) so output is scored beyond pass/fail. Calibrate the new judge dimensions via Phase 0.
- **Phase 2 — Trace flywheel** *(absorbs `subagent-eval-flywheel.md`; unblocked by Phase 0; feeds Phases 3–4)*. Harvest traces already on disk (`.session-context.archive/`, transcripts, `.claude/.agent-tokens.jsonl`) → **redacted** candidate cases under `_candidates/` → human attaches `referenceOutcome` + promotes via PR. Harvester emits **both** single-shot and trajectory candidates (the latter is Phase 3's raw input).
- **Phase 3 — Gap-2 autonomy-horizon trajectory eval** *(the biggest gap; consumes Phase-2 trajectories)*. Add an `evalKind: trajectory` case + a `trajectory[]` result shape (ordered steps); score it with deterministic objective checks (`step_budget`, `gate_recovery`, `no_repair_loop`) + a judge `goal_adherence` over the whole trajectory. First target: the orchestrator ship-loop on a known historical run.
- **Phase 4 — Gap-1 environment-complexity / multi-agent** *(most infra; gated behind Phase 3)*. Declarative `toolFaults[]` (rate-limit / timeout / ambiguous-doc injected on named tools) + a `coordination` expected-delegation graph; objective checks `delegation_correct` / `fault_handled`. Mock-tool fault layer in the runner.

The non-obvious trade-off: **objective-first, judge-sparingly.** Trajectory and multi-agent runs multiply judge non-determinism over many steps, so the new dimensions lean on deterministic structural checks (step counts, recovery events, loop detection, delegation-graph match) and reserve the calibrated judge for the one axis that genuinely needs it. That keeps the harness deterministic-where-it-can-be even as the eval surface grows to long-horizon agentic behaviour.

## Files to modify

Grouped by phase; each group is one PR. `path` links point at the existing files the phase edits (new files marked **new**).

**Phase 0 — calibration:**
1. `scripts/dev/agent-eval-calibrate.py` (**new**, pure stdlib) — consume a labelled corpus, compute κ / Spearman / Pearson / raw-agreement, emit a calibration report (markdown + JSON), propose per-dimension `max_score_drop` + `block` thresholds. No LLM; no third-party stats lib (κ + rank-correlation are ~40 lines of stdlib).
2. `docs/agent-eval/calibration-schema.json` (**new**) — one labelled record: `caseId`, `dimension`, `judgeScore`, `humanLabel`/`humanScore`, `raterId`, `runRef`.
3. `docs/agent-eval/calibration-set/code-review/*.json` (**new**) — seed labelled records over the 3 shipped `code-review` cases (human labels on real judge runs).
4. [`docs/agent-eval/scoring-policy.json`](../../agent-eval/scoring-policy.json) — add `block` (default `false`) + a `calibration` provenance block (`kappa`, `n`, `date`) per graduated dimension; flip the objective `code-review` dimensions to `block:true` once Phase-0 agreement clears the documented κ bar.
5. [`scripts/dev/agent-eval-score.py`](../../../scripts/dev/agent-eval-score.py) — honour `block`: a regression on a calibrated `block:true` dimension fails hard (the CI wrapper treats exit 1 as BLOCK for those); uncalibrated dimensions keep WARN. No change to the 0/1/2 contract — only which dimensions are *enforced*.
6. `tests/bats/agent_eval_calibrate.bats` (**new**) — fixture labelled set → assert κ value, threshold proposal, exit codes; deterministic, no LLM.
7. [`docs/agent-rules/subagent-eval.md`](../../agent-rules/subagent-eval.md) — document the calibration loop + the now-live WARN→BLOCK graduation criteria (κ bar, min-n).

**Phase 1 — Gap-3 output complexity:**
8. [`docs/agent-eval/case-schema.json`](../../agent-eval/case-schema.json) — extend the `check` enum with `uncertainty_flagged`, `escalation_present`; add `referenceOutcome.expectsEscalation` / `.expectsUncertainty` / `.artifacts[]`; add an optional `rubric` (weighted sub-criteria) to a `judge` dimension.
9. [`scripts/dev/agent-eval-score.py`](../../../scripts/dev/agent-eval-score.py) — implement the two new objective checks; pipe the `rubric` to the external judge and aggregate the weighted sub-scores.
10. [`docs/agent-eval/result-schema.json`](../../agent-eval/result-schema.json) — optional structured `trial.uncertainty` / `trial.escalation` fields the adapter parses.
11. `tests/agent-eval/code-review/*.json` (edit + **1 new**) — add the new dimensions to ≥1 existing case + a new case probing a "should-have-escalated / should-have-flagged-uncertainty" scenario (mined from a real `fix(` commit, same provenance rule).
12. `tests/bats/agent_eval_score.bats` (edit) — cover the new checks + rubric aggregation with the fake judge.

**Phase 2 — trace flywheel (absorbed from `subagent-eval-flywheel.md`):**
13. `scripts/dev/agent-eval-harvest.sh` (**new**) — scan `.session-context.archive/` + transcripts + `.claude/.agent-tokens.jsonl`; emit **redacted** candidate cases (single-shot **and** trajectory); harness-specific parse seam.
14. `tests/agent-eval/<agent>/_candidates/*.json` (**new staging dir**) — un-curated candidates awaiting human `referenceOutcome` + promotion.
15. `docs/agent-eval/harvest-policy.json` (**new**) — which agents to harvest, redaction patterns (token / email / key / Jira / p4 scrub), per-run candidate cap.
16. `tests/agent-eval/.harvest-ledger.json` (**new**) — dedup ledger of already-harvested trace IDs.
17. `tests/bats/agent_eval_harvest.bats` (**new**) — planted-fake-secret redaction test + dedup re-run test.
18. [`docs/agent-rules/subagent-eval.md`](../../agent-rules/subagent-eval.md) — document the harvest → curate → promote flywheel + the curation gate.

**Phase 3 — Gap-2 autonomy horizon:**
19. [`docs/agent-eval/case-schema.json`](../../agent-eval/case-schema.json) — add `evalKind` (`single-shot` default | `trajectory`); trajectory cases declare `expectedSteps` bounds, `requiredRecovery[]`, `forbiddenLoops[]`. **Schema wrinkle (grill-verified):** `referenceOutcome.expectedFindingCount` is currently `required` — a trajectory case scores steps, not findings, so it has none. Make the requirement conditional via an `if evalKind=trajectory then {required: [<trajectory ref fields>]} else {required: [expectedFindingCount]}` branch (the validator already resolves `if`/`then`), so a single-shot case still mandates `expectedFindingCount` while a trajectory case mandates its own reference shape (e.g. `expectedTerminalState`).
20. [`docs/agent-eval/result-schema.json`](../../agent-eval/result-schema.json) — add `trajectory[]` (ordered steps: `index`, `action`, `tool`, `observation`, `onTask`, `gateOutcome`); a trajectory result scores steps, not findings.
21. [`scripts/dev/agent-eval-score.py`](../../../scripts/dev/agent-eval-score.py) — trajectory objective checks `step_budget` / `gate_recovery` / `no_repair_loop` + a judge `goal_adherence` over the trajectory.
22. [`scripts/dev/agent-eval-run.sh`](../../../scripts/dev/agent-eval-run.sh) — trajectory runner mode: drive the orchestrator through a multi-step scenario, capture the step trajectory via the adapter seam.
23. `tests/agent-eval/orchestrator/*.json` (**new**) — trajectory cases for the ship-loop (anchored to a real historical run).
24. `tests/bats/agent_eval_run.bats` + `agent_eval_score.bats` (edit) — `--fake-runner` trajectory fixture; trajectory-check assertions.

**Phase 4 — Gap-1 environment complexity / multi-agent:**
25. [`docs/agent-eval/case-schema.json`](../../agent-eval/case-schema.json) — add `toolFaults[]` (declarative rate-limit / timeout / ambiguous-doc on named tools) + a `coordination` expected-delegation graph.
26. [`scripts/dev/agent-eval-run.sh`](../../../scripts/dev/agent-eval-run.sh) — mock-tool fault layer that returns injected faults; capture which subagents the orchestrator spawned.
27. [`scripts/dev/agent-eval-score.py`](../../../scripts/dev/agent-eval-score.py) — objective checks `delegation_correct` (right specialists spawned) + `fault_handled` (retried/escalated, no crash/loop).
28. `tests/agent-eval/orchestrator/*.json` (edit + **new**) — multi-agent + injected-fault cases.
29. `tests/bats/agent_eval_run.bats` (edit) — fault-injection + delegation-capture assertions with the fake runner.

**Cross-cutting (done in THIS plan-doc PR — the absorb; the rest land per-phase above):**
30. `docs/plans/subagent-eval-flywheel.md` — set `Status` to `deferred — superseded by subagent-eval-agentic-coverage`; add a one-line redirect banner. (Folded, not cancelled — its content lives in Phase 2 here.) **Done in this PR.**
31. [`docs/agent-rules/subagent-eval.md`](../../agent-rules/subagent-eval.md) line 5 — repoint the forward-roadmap pointer from the standalone flywheel plan to this unified plan (tier-less). **Done in this PR.** (AGENTS.md needs no edit: it references subagent-eval only via the `cpp-rules.md` keyword row — the nav chain is `AGENTS.md` → `cpp-rules.md` → `subagent-eval.md`, and the load-bearing pointer is the one in `subagent-eval.md`.) The deeper Phase-0 doc edit — documenting the calibration loop + WARN→BLOCK criteria — is item #7 above and lands with Phase 0.
32. `docs/self-improvement/categories/tooling.md` (2026-05-31 P2 residue) — repoint its item-(3) flywheel reference to this unified plan; entry stays **open** until Phase 0 actually ships (then partly retire it). **Done in this PR.**

## Existing utilities reused

- `scripts/dev/agent-eval-score.py` — the shipped pure-stdlib scorer + external-judge seam + 0/1/2 exit contract; every new dimension/check extends it, doesn't fork it.
- `scripts/dev/agent-eval-run.sh` — the shipped runner + `--prompt-root` before/after seam + `--fake-runner` no-token path + harness-adapter seam; trajectory + fault modes are new run-modes behind the same seams.
- `docs/agent-eval/{case,result}-schema.json` — extended (new enums / optional fields), version-bumped only on a breaking shape change (both are `schemaVersion: 1` today). **Gotcha (grill-verified):** the case schema sets `additionalProperties: false` on the case object, every `dimension`, and `referenceOutcome` — so each new field (Phase 1's `expectsEscalation` / `rubric` / …, Phase 3's `evalKind` / trajectory bounds, Phase 4's `toolFaults[]` / `coordination`) **must be added to the schema**, not merely emitted; an un-declared key fails validation. The `check` enum (currently `cited_file_line` / `severity_enum` / `finding_count`) likewise must be widened in-place per phase.
- `docs/agent-eval/scoring-policy.json` `default` + `perDimension` precedence — the `block` flag + `calibration` provenance slot into the existing override structure.
- `tests/agent-eval/validate_schema.py` — the shipped stdlib draft-07-subset validator (resolves `$ref` / `allOf` / `if`/`then`); validates the extended schemas + new case files; no third-party validator.
- `perf-compare.py` skeleton — the calibration report emitter mirrors its `evaluate` / `emit_markdown` shape one more level up.
- Trace sources for the flywheel (storage-substrate-verified against the main integration tree, 2026-06-07) — `.session-context.archive/` (dated scratchpad-archive `.md` files; the `scratchpad-recall` skill reads them), `.claude/.agent-tokens.jsonl` (per-agent token log written/queried by the `agents/_shared/token-tracking/` module — `agent-token-log.py` + `agents/scripts/core/agent-tokens-report.py`; tells which agent ran), and session transcripts under `~/.claude/projects/<slug>/`. All three are gitignored / session-local — present in an active tree, **absent in a fresh worktree** (the harvester must tolerate a missing source, not assume it).
- `agents/scripts/core/test-shell-lint.sh` — gates `agent-eval-harvest.sh` + the runner edits.
- `agents/scripts/core/setup-harness.sh` + `AGENTS.md` § Harness adapter — harvester + trajectory capture are per-harness; emitted formats stay portable.
- `tests/bats/merge_gates.bats` + the two shipped `agent_eval_*.bats` — bats prior art for the new suites.

## UX Pillar callouts

Dev-process / eval tooling only — no product-runtime code, no `Source/Core/` change. All four N/A (same rationale as both parent plans).

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: N/A — runs offline in CI / pre-push, never on the UI thread.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: N/A — no product code path touched.
- **Pillar 3 (never crash)**: N/A — scorer/calibrator are pure Python; runner + harvester shell out in separate processes.
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

`N/A` — no `Source/Core/` code, no C++. Every phase's diff is `scripts/` + `tests/` + `docs/` (+ one-line `AGENTS.md`) — pure-docs / tooling-shell, allow-listed by `agents/scripts/core/is-pure-docs-diff.sh` (`*.md` + `scripts/dev/**` + `tests/**` + `agents/scripts/**`); build / ctest / perf gates skip. Verification is shell-lint + bats + schema conformance + (Phase 2) security-review.

1. **PR-fast CI** — N/A (no perf-gated path). 2. **Pillar 2 static scanner** — N/A (no sync-I/O reachable from `ImGui::*`). 3. **Dispatcher drain** — N/A. 4. **Visible-cue bucket-E harness** — N/A. 5. **Marker inventory** — N/A (no `SMATCHET_UI_PERF_SCOPE` markers).

## Risks / non-goals

**Risks:**

- **Calibration corpus cost / small-n** — human labels are expensive and the seed corpus is small (3 `code-review` cases). *Mitigation*: graduate the **objective** dimensions first (zero judge noise — agreement is trivially high), document an explicit κ bar + min-n before any judge dimension graduates, and let Phase 2's flywheel grow the labelled corpus over time. A dimension stays advisory until it clears the bar — no premature BLOCK.
- **Judge non-determinism amplified over trajectories** — a judge over a hundreds-of-step run is noisier than over one finding list. *Mitigation*: objective-first design — `step_budget` / `gate_recovery` / `no_repair_loop` / `delegation_correct` / `fault_handled` are all deterministic structural checks; the judge is reserved for the single `goal_adherence` axis, multi-trial averaged.
- **Trajectory / transcript-shape coupling** — step capture reads Claude-Code-specific transcript structure. *Mitigation*: isolate the parse in the harness-adapter seam (same discipline as the shipped runner); the `trajectory[]` schema + scoring stay portable.
- **Fault-injection mock fidelity** (Phase 4) — a mock tool layer can diverge from real tool failure modes. *Mitigation*: start with the two highest-signal, easily-faithful faults (rate-limit `429` + timeout); declarative `toolFaults[]` keeps cases readable; ambiguous-doc fault deferred until the simple two prove out.
- **SECURITY — secret / PII leakage in harvested traces** (Phase 2, primary risk) — transcripts + archives can carry tokens, keys, emails, Jira / p4 content. *Mitigation*: mandatory redaction pass **before any candidate is written**; candidates stage in `_candidates/` and enter the committed set only through human PR review; the `security-review` agent runs on the harvester before it ships; the redaction bats test plants fake secrets and asserts they never reach an emitted candidate. (Inherited verbatim from the absorbed flywheel plan.)
- **Scope / size** — five phases is a large body of work. *Mitigation*: strict prove-first phasing — each phase is an independently shippable PR with standalone value; Phase 0 alone retires the open P2 residue and graduates the first BLOCK; Phase 4 is explicitly gated behind Phase 3 demonstrating trajectory eval earns its keep.
- **Over-build of rarely-run heavy auditors** — trajectory + fault harnesses are token-heavy on-demand tools. *Mitigation*: they run on-demand (prompt-PR scoring + manual smoke), never per-commit in CI; the `--fake-runner` path keeps CI deterministic + tokenless.

**Non-goals:**

- **NON-GOAL — auto-promotion of harvested candidates**: candidates never enter the scored set without a human attaching `referenceOutcome` + PR review. No exceptions.
- **NON-GOAL — online / continuous eval + dashboard**: on-demand commands only; an internal agent fleet doesn't warrant live-sampling / observability infra (Phase 3 in the talk's maturity model — no-action).
- **NON-GOAL — Smatchet product / in-app AI-assistant eval**: separate plan, pending a scope check of the `AGENTS.md` security-review "AI-assistant" surface.
- **NON-GOAL — coverage of every one of the ~30 agents**: this plan covers `code-review` (existing), the **orchestrator** (trajectory + multi-agent), and whatever agents the flywheel harvests; broad per-agent coverage is incremental follow-up.
- **NON-GOAL — replacing the perf eval**: the perf pipeline (`perf-run.sh` / `perf-compare.py`) is the latency dimension; this is the decision-quality dimension. Sibling systems, not a merge.

## Verification

Per `AGENTS.md` § Verification automation. Not C++ — Buckets A/E N/A. Everything is verifiable **without a live agent** via fake-judge + `--fake-runner` fixtures; live smokes are explicitly tracked as deferred-automation residue, not silent.

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no `Source/Core/` helper added.
- **Bucket E (ImGui Test Engine)**: N/A — no UI.
- **Bash-driver / bats per phase**:
  - Phase 0 — `tests/bats/agent_eval_calibrate.bats`: fixture labelled set → asserts κ / correlation values, threshold-proposal output, and the `block`-enforcement exit behaviour in `agent-eval-score.py` (calibrated `block:true` regression fails; uncalibrated WARNs). Deterministic, no LLM.
  - Phase 1 — `agent_eval_score.bats` (extended): fake judge → asserts the `uncertainty_flagged` / `escalation_present` objective checks + weighted-rubric aggregation.
  - Phase 2 — `agent_eval_harvest.bats`: planted fake secrets → asserts redaction scrubs them from emitted candidates; a second run over the same trace → asserts the dedup ledger blocks a duplicate.
  - Phase 3 — `agent_eval_run.bats` + `agent_eval_score.bats` (extended): `--fake-runner` trajectory fixture conforms to `result-schema.json`; `step_budget` / `gate_recovery` / `no_repair_loop` assertions on a canned trajectory; no live tokens.
  - Phase 4 — `agent_eval_run.bats` (extended): injected `toolFaults[]` surface through the mock layer; `delegation_correct` / `fault_handled` assertions on a canned multi-agent run.
- **Schema conformance**: every extended/new case + a sample of each new result shape validate via `tests/agent-eval/validate_schema.py` (stdlib); a negative control (missing-field doc rejected) per phase.
- **Shell-lint**: `agent-eval-harvest.sh` + the `agent-eval-run.sh` edits pass `agents/scripts/core/test-shell-lint.sh`.
- **Security review (Phase 2)**: the `security-review` agent runs on `agent-eval-harvest.sh` before merge — redaction completeness, path-traversal on trace globs, no secret echo to logs.
- **Build gate**: `N/A` — no compile (tooling/docs diff; `is-pure-docs-diff.sh` classifies each phase pure-docs/tooling).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script, don't hardcode sub-steps). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the eval-harness domain model + sharpen terms (trajectory / evalKind / calibration / block-graduation / coordination-graph) before finalising; record the outcome. Required — do not delete.
- **Manual residue**: the **live smokes** are inherently manual and tracked, not silent — (a) one live `code-review` end-to-end smoke (real `claude -p` adapter + real judge) producing a conformant result; (b) labelling the seed calibration corpus (human-in-the-loop by definition); (c) one live orchestrator trajectory capture to confirm the adapter parses real step logs. Deferred-automation action plan: extend the existing `docs/self-improvement/categories/tooling.md` 2026-05-31 entry (which Phase 0 partly retires) with the per-phase smoke cadence. No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: this plan **absorbs** `subagent-eval-flywheel.md` (folded into Phase 2, not deferred), so the residue action is a **supersede, not a delete** — mark that file `deferred — superseded`, add a redirect banner, regen the plan index (`test-plan-index.sh --fix`), and repoint the most load-bearing pointer (`docs/agent-rules/subagent-eval.md` line ~5) at this unified plan. Grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray `subagent-eval-flywheel` "deferred-as-current" refs and repoint them here. Leave historical references in `docs/plans/shipped/subagent-eval-harness.md` (immutable shipped record) intact.

- **Coverage of the remaining ~28 agents** (`debug-detective`, `perf-detective`, `tracker-backend`, …) — after the orchestrator + `code-review` dimensions prove the pattern; incremental, one agent per follow-up.
- **Online / continuous eval + dashboard (talk maturity Phase 3)** — no-action; on-demand only.
- **Ambiguous-doc tool-fault class (Phase 4)** — deferred until rate-limit + timeout faults prove the mock layer; named, not designed.
- **Aggregate single-score-per-agent rollup** — the `weight` field exists in `case-schema.json` reserved for this; the MVP + this plan score per-dimension. Follow-up once calibration trust is established across dimensions.
- **Product / in-app AI-assistant eval** — separate plan (security-review "AI-assistant" surface scope check first).

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. In the SAME PR that populates the three sections above (once ALL five phases have shipped and every cited PR is merged) —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/subagent-eval-agentic-coverage.md docs/plans/shipped/` (move into the shipped tier),*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*References use the tier-less form `docs/plans/<slug>.md` so the move can't break them. Write new references tier-less. Delete this `## Archive` block as part of step 2.*
