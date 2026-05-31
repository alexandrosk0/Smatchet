# Plan — Subagent eval harness (eval-driven development for the dev-agent fleet)

> **Slug**: `subagent-eval-harness` (matches this file's basename without `.md`).
>
> **Scope clarifier**: this evaluates the **development agents** (`agents/*.md` — orchestrator + the ~30 delegated subagents), NOT the Smatchet product or any in-app AI-assistant surface. See § Out of scope.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

The dev process mutates agent prompts (`agents/*.md`) through the self-improvement loop (`docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`) with **zero before/after measurement** — prompt edits ship on judgment, not data. There is no way to know whether a prompt edit made `code-review` catch more real findings or quietly regressed `debug-detective` on cases it used to nail.

Meanwhile the **perf dimension already runs full eval-driven development**: a frozen scenario set → `scripts/dev/perf-run.sh` → `scripts/dev/perf-compare.py` diff vs a baseline → threshold gate that blocks regressions. That is exactly the "optimize against evals" loop, just for one dimension (latency).

Prompted by the video *"The maturity phases of running evals"* (Phil Hetzel, Braintrust, <https://www.youtube.com/watch?v=FB-MLPhL9Ms>). Core lessons applied here: evals ≠ unit tests (dimensional scoring, not exact-match pass/fail); measure every change against a frozen golden set; block scored regressions; grow the golden set from real traces ("flywheel"); calibrate the auto-judge against humans.

**Intended outcome (one sentence):** after this lands, editing a covered agent's prompt runs that agent's frozen golden case set and surfaces a scored, dimensional before/after delta that blocks regressions — the same gate shape the perf dimension already enjoys.

## Approach

Mirror the perf pipeline one level up the stack — agent **decision quality** instead of frame latency — reusing the perf-gate shape verbatim wherever possible. Three artifacts:

1. **Frozen golden case sets** — `tests/agent-eval/<agent>/*.json`. Each case = `{ input, referenceOutcome, dimensions[] }` (input = a frozen diff / bug repro / scenario; reference = the finding/diagnosis that actually mattered; dimensions = the rubric axes to score). Seeded from **real prior runs** harvested out of session archives + transcripts.
2. **Runner** — `scripts/dev/agent-eval-run.sh <agent> <case>`: invokes ONE agent headlessly (`claude -p` print mode / Agent SDK), runs `N` trials, captures the agent's final output to a result JSON. This is the only harness-coupled piece — a thin adapter, swappable per harness exactly like `scripts/setup-harness.sh`.
3. **Scorer** — `scripts/dev/agent-eval-score.py`: LLM-as-judge with a calibrated rubric **plus** code-based checks (e.g. "did the finding cite the correct `file:line`?"), emits per-dimension scores, a markdown delta table, and exits non-zero on a threshold breach vs `docs/agent-eval/scoring-policy.json`. Pure Python stdlib, harness-agnostic — a near-direct clone of `perf-compare.py`.

Wire into the self-improvement loop: a PR that edits `agents/<name>.md` for a covered agent runs that agent's case set before/after and reports the scored delta; a regression **WARNs** initially (label-overridable, like `perf-out-of-band`) and graduates to **BLOCK** once the judge is calibrated.

**Key trade-off:** agents are non-deterministic LLMs, not deterministic C++ scenarios. Handled with multi-trial averaging + tolerance thresholds (the video's "run across multiple trials") and LLM-as-judge scoring instead of exact match — never a single-run binary gate. The non-determinism is contained to the runner; the case format and scorer stay deterministic and portable.

**Phasing.** **Phase 1 (MVP)** — 3 highest-leverage diagnostic agents, offline, manual / PR-scoped trigger, hand-seeded golden cases. **Phase 2 (in scope) — trace flywheel**: grow the golden set automatically from real session traces (the video's "production traces become eval datasets"), sequenced after Phase 1's judge is calibrated. **Phase 3 deferred** — online / continuous eval + dashboard (see § Out of scope).

**Phase 2 mechanism.** A harvester (`scripts/dev/agent-eval-harvest.sh`) scans traces that **already exist** — `.session-context.archive/`, session transcripts, `.claude/.agent-tokens.jsonl` (tells which agent ran) — for runs of covered agents, extracts `(input, agent-output)` pairs, **redacts secrets / PII**, and emits them as *candidate* cases under `tests/agent-eval/<agent>/_candidates/`. Candidates are **proposals, not golden**: a human attaches the `referenceOutcome` and promotes them into the live set via PR review. This keeps the gate deterministic (only curated cases score) while letting the dataset grow from reality. A dedup ledger (`tests/agent-eval/.harvest-ledger.json`) blocks re-harvesting the same trace. No trace-storage infra is built — the harvester reads archives already on disk.

## Files to modify

Create (new subsystem — grouped by role):

*Case data*
1. `tests/agent-eval/code-review/*.json` — seed set: 5-10 frozen diffs + the findings that mattered.
2. `tests/agent-eval/debug-detective/*.json` — seed set: frozen bug repros + known root cause.
3. `tests/agent-eval/perf-detective/*.json` — seed set: frozen scenario + known hot path.

*Pipeline*
4. `scripts/dev/agent-eval-run.sh` — headless single-agent runner; mirror `perf-run.sh` CLI shape (`<agent> <case> [--trials=N] [--out=<path>]`).
5. `scripts/dev/agent-eval-score.py` — dimensional scorer + threshold gate; mirror `perf-compare.py` (load/extract/evaluate/emit-markdown, exit 0/1/2).
6. `docs/agent-eval/scoring-policy.json` — per-dimension thresholds + per-agent overrides; mirror `docs/perf/regression-policy.json`.
7. `docs/agent-eval/case-schema.json` — case-file schema; mirror `scripts/dev/perf-baseline-schema.json`.

*Wiring + tests*
8. `docs/agent-rules/subagent-eval.md` — the rule: when the gate fires, WARN→BLOCK graduation, how the self-improvement loop consumes the delta.
9. `tests/bats/agent_eval_score.bats` — scorer unit tests against fixed input/reference JSON (judge mocked → deterministic); mirror `tests/bats/merge_gates.bats`.
10. `AGENTS.md` — one-line § Project rules pointer + stub to `docs/agent-rules/subagent-eval.md`.
11. `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` — wire the eval gate into the loop as the "optimize against evals" step.

*Phase 2 — trace flywheel*
12. `scripts/dev/agent-eval-harvest.sh` — scan `.session-context.archive/` + transcripts + `.claude/.agent-tokens.jsonl` → redacted *candidate* cases; harness-specific adapter like the runner.
13. `tests/agent-eval/<agent>/_candidates/*.json` — staging for un-curated harvested candidates (await human `referenceOutcome` + promotion).
14. `docs/agent-eval/harvest-policy.json` — which agents to harvest, redaction patterns (token / email / key scrub), per-run candidate cap.
15. `tests/agent-eval/.harvest-ledger.json` — dedup ledger of already-harvested trace IDs.
16. `tests/bats/agent_eval_harvest.bats` — redaction + dedup tests (fixture transcript with planted fake secrets → assert scrubbed; re-run → assert no dup).

(Item 8 `docs/agent-rules/subagent-eval.md` also documents the harvest → curate → promote flywheel + the curation gate.)

## Existing utilities reused

- `perf-compare.py` skeleton (`load_json` / `extract_rows` / `evaluate` / `emit_markdown` + 0/1/2 exit-code contract) — `scripts/dev/perf-compare.py:62` onward — clone the scorer structure verbatim, swap perf rows for dimension scores.
- `perf-run.sh` arg-parse + output-path discipline (stale-file wipe, last-line-is-path) — `scripts/dev/perf-run.sh:54` — clone the runner shell shape.
- `regression-policy.json` `default` + `perScenario` override pattern — `docs/perf/regression-policy.json` (consumed at `scripts/dev/perf-compare.py:74`) — reuse as `default` + `perAgent`.
- Harness-adapter philosophy (per-harness adapter, portable core) — `scripts/setup-harness.sh` + `AGENTS.md` § Harness adapter — runner is per-harness, case format + scorer portable.
- Trace source for case harvesting (Phase 2) — `.session-context.archive/` (via `scratchpad-recall` skill) + `.claude/.agent-tokens.jsonl` (via `agent-tokens` skill) + session transcripts.
- `claude -p` headless print mode — the runner's agent-invocation mechanism.

## UX Pillar callouts

Dev-process tooling only — no product-runtime code. All four N/A.

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: N/A — no `Source/Core/` change; the harness runs offline in CI / pre-push, never on the UI thread.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: N/A — same; no product code path touched.
- **Pillar 3 (never crash)**: N/A — scorer is pure Python; runner shells the harness in a separate process.
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

`N/A` — no `Source/Core/` change. The **planned implementation** touches `scripts/`, `tests/`, `docs/`, `AGENTS.md` only; this plan PR itself is docs-only. (Implementation will be a tooling diff — runs shell-lint + bats, skips the perf gate.)

## Risks / non-goals

- **RISK — non-determinism**: LLM agents vary run-to-run → flaky gate. *Mitigation*: N-trial averaging + tolerance thresholds + a min-trials floor (mirror `min_baseline_calls`); gate starts as WARN, graduates to BLOCK only after calibration.
- **RISK — token cost**: each eval run spends real tokens (spawns a live agent). *Mitigation*: tiny case sets (5-10), manual / PR-scoped trigger only (NOT every push), 3 agents not 30.
- **RISK — judge drift**: LLM-as-judge mis-scores. *Mitigation*: periodic human calibration (the video's Phase-3 step); keep code-based checks wherever objective (`file:line` match, severity enum, finding count).
- **RISK — harness coupling**: runner needs `claude -p`. *Mitigation*: isolate it in the `.sh` adapter; case format + scorer stay portable per § Harness adapter — a Codex/Cursor runner is a drop-in sibling.
- **SECURITY — secret / PII leakage (Phase 2)**: session transcripts + archives can carry tokens, API keys, emails, Jira / p4 content. *Mitigation*: mandatory redaction pass in the harvester before any candidate is written; candidates stage in `_candidates/` and enter the committed set only through human PR review; run `security-review` on the harvester before it ships.
- **RISK — curation burden / candidate quality (Phase 2)**: auto-harvested traces are noisy. *Mitigation*: harvester emits *proposals* only — a human attaches `referenceOutcome` and promotes; a per-run candidate cap keeps review tractable.
- **RISK — trace-format coupling (Phase 2)**: transcript shape is Claude-Code-specific. *Mitigation*: harvester is harness-specific like the runner; isolate the parse seam, keep the emitted case format portable.
- **NON-GOAL**: evaluating the Smatchet **product** / its in-app AI-assistant surface — separate plan if that surface ships LLM features (see § Out of scope).
- **NON-GOAL**: building trace-storage / observability infra (Braintrust-scale) — overkill for an internal fleet; the flywheel reuses existing archives instead.
- **NON-GOAL**: covering all ~30 agents — MVP is the 3 highest-leverage diagnostic ones only.

## Verification

Not C++ — Bucket A/E mostly N/A.

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no `Source/Core/` helper added.
- **Bucket E (ImGui Test Engine)**: N/A — no UI.
- **Bash-driver / scorer test**: `tests/bats/agent_eval_score.bats` runs the scorer against fixed input + reference JSON with the judge mocked (deterministic), asserting the exit-code contract (0 clean / 1 regression / 2 malformed) and the markdown delta shape — mirrors `merge_gates.bats`. `scripts/dev/agent-eval-run.sh` passes `scripts/dev/test-shell-lint.sh` (5-rule checklist).
- **Harvester test (Phase 2)**: `tests/bats/agent_eval_harvest.bats` feeds a fixture transcript with planted fake secrets → asserts redaction scrubs them from emitted candidates; a second run over the same trace asserts the dedup ledger blocks a duplicate. `agent-eval-harvest.sh` passes `test-shell-lint.sh`.
- **Smoke**: run the full loop once end-to-end on `code-review`'s seed set; eyeball the markdown delta + dimension scores for sanity.
- **Build gate**: N/A — no compile (`SmatchetStandalone` / `SmatchetCore_DX12` untouched).
- **Manual residue**: judge-vs-human calibration is inherently manual (the video names this as a permanent step). Deferred-automation action plan: add a `docs/self-improvement/categories/tooling.md` entry tracking calibration cadence (e.g. re-calibrate every N new cases). Not silent residue.

## Out of scope (flagged, not designed)

- **Phase 3 — online / continuous eval + dashboard**: no-action. An internal fleet doesn't warrant live sampling / observability infra (explicit non-goal above).
- **Product AI-assistant eval**: separate plan, pending a scope check of whatever the `AGENTS.md` § security-review "AI-assistant" surface actually is.
- **Coverage past 3 agents**: backlog after the MVP proves the gate catches a real prompt regression.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
