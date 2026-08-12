# Plan — Subagent eval harness (eval-driven development for the dev-agent fleet)

> **Slug**: `subagent-eval-harness` (matches this file's basename without `.md`).
>
> **Scope clarifier**: this evaluates the **development agents** (`agents/*.md` — orchestrator + the ~30 delegated subagents), NOT the Smatchet product or any in-app AI-assistant surface. See § Out of scope.
>
> **Scope (this plan)**: Phase 1 MVP only — **prove the scoring contract before any live agent, harvester, or merge-gate block**. `code-review` only. The trace flywheel (Phase 2) is split into a separate follow-up plan: `docs/plans/subagent-eval-flywheel.md`.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

The dev process mutates agent prompts (`agents/*.md`) through the self-improvement loop (`docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`) with **zero before/after measurement** — prompt edits ship on judgment, not data. There is no way to know whether a prompt edit made `code-review` catch more real findings or quietly regressed `debug-detective` on cases it used to nail.

Meanwhile the **perf dimension already runs full eval-driven development**: a frozen scenario set → `scripts/dev/perf-run.sh` → `scripts/dev/perf-compare.py` diff vs a baseline → threshold gate that blocks regressions. That is exactly the "optimize against evals" loop, just for one dimension (latency).

Prompted by the video *"The maturity phases of running evals"* (Phil Hetzel, Braintrust, <https://www.youtube.com/watch?v=FB-MLPhL9Ms>). Core lessons applied here: evals ≠ unit tests (dimensional scoring, not exact-match pass/fail); measure every change against a frozen golden set; block scored regressions; grow the golden set from real traces ("flywheel" — deferred to the follow-up plan); calibrate the auto-judge against humans.

**Intended outcome (one sentence):** after this lands, the scoring contract is proven end-to-end against deterministic fixtures — a `code-review` prompt edit can be scored base-vs-head across quality dimensions — with **advisory** CI output, ready to graduate to a blocking gate once calibration data exists.

## Approach

Mirror the perf pipeline one level up the stack — agent **decision quality** instead of frame latency — reusing the perf-gate shape wherever possible. The decisive choice for the MVP: **prove the scoring contract against fixtures before spending a single live-agent token.** Build strictly in this order; each step is testable without the next.

**1 — Contract (schemas).** Three JSON schemas pin the data shape: `case-schema.json` (a golden case), `result-schema.json` (one runner output), `scoring-policy.json` (per-dimension thresholds + per-agent overrides). Nothing else is built until these are fixed.

**2 — Scorer (`agent-eval-score.py`), pure Python stdlib.** Consumes a base result JSON + a head result JSON (per `result-schema`), emits a per-dimension markdown delta, exits `0/1/2` — a near-direct clone of `perf-compare.py`. It is **pure-stdlib** because it never calls an LLM directly: dimensional grading goes through an **external judge command** named by `--judge-cmd` / `$SMATCHET_AGENT_EVAL_JUDGE_CMD` (the scorer pipes `{case, output}` in, parses a JSON score out); objective dimensions (`file:line` cited, severity enum, finding count) are code-based checks inline. The judge being external is what keeps the scorer deterministic and unit-testable.

**3 — Scorer tests (`agent_eval_score.bats`).** Inject a **fake judge command** that echoes canned scores → fully deterministic. Assert the exit-code contract (0 clean / 1 regression / 2 malformed) and the delta-table shape. This proves the contract with **no live agent**.

**4 — Curated cases (`code-review` only, 3-5).** Hand-picked from real prior runs. Each case carries enough **repo context to reproduce the run** (see § Case metadata).

**5 — Runner (`agent-eval-run.sh`), only after the scorer is stable.** Invokes ONE agent for ONE case, `N` trials, writes a `result-schema` JSON. Two non-negotiable seams:
- **Before/after** is explicit via `--prompt-root=<path>`: the runner reads the agent-prompt tree from that path, so a prompt PR is scored by running the case set once with `--prompt-root=<base worktree>` and once with `--prompt-root=<head worktree>`, then diffing the two result JSONs (mirrors perf base-vs-fresh). No implicit "current checkout".
- **Harness adapter**: `claude -p` print mode is **one** adapter behind a seam, not the canonical interface; the case / result / scoring formats assume no specific harness. A `--fake-runner` mode emits a canned `result-schema` JSON so the runner is bats-testable with **no live tokens in CI**.

**6 — Wiring, advisory-only.** `subagent-eval.md` rule + `AGENTS.md` stub + self-improvement-loop hook. CI is **advisory at first**: a malformed eval artifact **FAILs**; a quality regression only **WARNs** until enough calibration data exists to trust the judge. The WARN→BLOCK graduation (and any merge-gate wiring) is explicitly out of the MVP.

### Case metadata

A frozen diff alone is not reproducible. Each `code-review` case records: `repoRef` (commit SHA), `baseBranch`, the relevant files / diff, `agent` name, `toolPosture` (read-only vs edit), and the **exact delegation packet** handed to the agent. The runner reconstructs the run from these fields.

## Files to modify

Create (new subsystem — listed in build order; the order is the point):

1. `docs/agent-eval/case-schema.json` — golden-case format: `repoRef`, `baseBranch`, files/diff, `agent`, `toolPosture`, `delegationPacket`, `dimensions[]`, `referenceOutcome`.
2. `docs/agent-eval/result-schema.json` — one runner output: per-trial agent final output + run metadata; the contract the scorer consumes.
3. `docs/agent-eval/scoring-policy.json` — per-dimension thresholds + per-agent overrides; mirror `docs/perf/regression-policy.json`.
4. `scripts/dev/agent-eval-score.py` — pure-stdlib scorer; external judge via `--judge-cmd` / `$SMATCHET_AGENT_EVAL_JUDGE_CMD` + inline code-checks; base-vs-head delta; exit `0/1/2`; mirror `perf-compare.py`.
5. `tests/bats/agent_eval_score.bats` — scorer tests with a **fake judge command** (deterministic); mirror `tests/bats/merge_gates.bats`.
6. `tests/agent-eval/code-review/*.json` — **3-5 curated cases, `code-review` only.**
7. `scripts/dev/agent-eval-run.sh` — runner, **after** the scorer is stable; `--prompt-root` (base/head), `--trials=N`, `--fake-runner`; `claude -p` as one adapter; mirror `perf-run.sh` CLI shape.
8. `docs/agent-rules/subagent-eval.md` — the rule: malformed artifact = FAIL, quality regression = WARN (advisory) until calibrated; documents the deferred WARN→BLOCK graduation.
9. `AGENTS.md` — one-line § Project rules pointer + stub to `docs/agent-rules/subagent-eval.md`.
10. `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` — wire the advisory eval delta into the loop as the "optimize against evals" step.

No harvester / flywheel artifacts here — see § Out of scope.

## Existing utilities reused

- `perf-compare.py` skeleton (`load_json` / `extract_rows` / `evaluate` / `emit_markdown` + 0/1/2 exit-code contract) — `scripts/dev/perf-compare.py:62` onward — clone the scorer structure, swap perf rows for dimension scores.
- `perf-run.sh` arg-parse + output-path discipline (stale-file wipe, last-line-is-path) — `scripts/dev/perf-run.sh:54` — clone the runner shell shape.
- `regression-policy.json` `default` + `perScenario` override pattern — `docs/perf/regression-policy.json` (consumed at `scripts/dev/perf-compare.py:74`) — reuse as `default` + `perAgent`.
- Harness-adapter philosophy (per-harness adapter, portable core) — `agents/scripts/core/setup-harness.sh` + `AGENTS.md` § Harness adapter — runner is per-harness, case / result / scoring formats portable.
- Shell-lint gate — `agents/scripts/core/test-shell-lint.sh` (5-rule checklist) — the runner must pass it.
- `merge_gates.bats` bats prior art — `tests/bats/merge_gates.bats`.
- `claude -p` headless print mode — **one** runner adapter, behind the harness seam (not the canonical interface).

## UX Pillar callouts

Dev-process tooling only — no product-runtime code. All four N/A.

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: N/A — no `Source/Core/` change; the harness runs offline in CI / pre-push, never on the UI thread.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: N/A — same; no product code path touched.
- **Pillar 3 (never crash)**: N/A — scorer is pure Python; runner shells the harness in a separate process.
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

`N/A` — no `Source/Core/` change. The **planned implementation** touches `scripts/`, `tests/`, `docs/`, `AGENTS.md` only; this plan PR itself is docs-only. (Implementation will be a tooling diff — runs shell-lint + bats, skips the perf gate.)

## Risks / non-goals

- **RISK — before/after underspecified** (review finding 1): a single checkout can't produce a delta. *Mitigation*: explicit `--prompt-root=<path>` base-vs-head; the two-worktree recipe is documented in `subagent-eval.md`.
- **RISK — scorer purity vs LLM judge** (review finding 2): can't be pure-stdlib *and* call an LLM. *Mitigation*: external judge command (`--judge-cmd` / `$SMATCHET_AGENT_EVAL_JUDGE_CMD`); scorer stays pure-stdlib + deterministic; tests use a fake judge.
- **RISK — harness coupling** (review finding 4): `claude -p` is Claude-specific. *Mitigation*: it is one adapter behind a seam; case / result / scoring formats are harness-agnostic. (Path corrected: `agents/scripts/core/setup-harness.sh`.)
- **RISK — non-determinism**: live agents vary run-to-run. *Mitigation*: scoring contract is proven against fixtures first; `--fake-runner` + fake judge keep CI deterministic with no live tokens; multi-trial averaging + tolerance for live runs.
- **RISK — cases not reproducible** (review finding 5): a frozen diff alone is insufficient. *Mitigation*: case schema records `repoRef` / `baseBranch` / files / `agent` / `toolPosture` / `delegationPacket`.
- **NON-GOAL — no live merge-gate BLOCK in the MVP**: CI is advisory (malformed = FAIL, regression = WARN) until calibration data exists. WARN→BLOCK is deferred.
- **NON-GOAL — Phase 2 trace flywheel** (review finding 3): harvest / redaction / dedup / candidates / ledger are a separate, security-sensitive plan (`subagent-eval-flywheel.md`).
- **NON-GOAL — coverage beyond `code-review`**: added only after the MVP proves the contract.
- **NON-GOAL — Smatchet product / in-app AI-assistant eval**: separate plan.

## Verification

Not C++ — Bucket A/E N/A. Everything in the MVP is verifiable **without a live agent**.

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no `Source/Core/` helper added.
- **Bucket E (ImGui Test Engine)**: N/A — no UI.
- **Scorer test (`tests/bats/agent_eval_score.bats`)**: fake judge command + fixed base/head result JSON → asserts exit-code contract (0/1/2) and delta-table shape. Deterministic, no live agent.
- **Runner test (`tests/bats/agent_eval_run.bats`)**: `--fake-runner` mode → asserts the emitted JSON conforms to `result-schema.json`; no live tokens.
- **Schema conformance**: the 3-5 curated cases + a sample result validate against `case-schema.json` / `result-schema.json` (stdlib check inside the bats, no third-party validator).
- **Shell-lint**: `scripts/dev/agent-eval-run.sh` passes `agents/scripts/core/test-shell-lint.sh`.
- **Build gate**: N/A — no compile.
- **Manual residue**: one live `code-review` end-to-end smoke (off-CI) once scorer + runner are stable, and judge-vs-human calibration, are inherently manual. Deferred-automation action plan: a `docs/self-improvement/categories/tooling.md` entry tracking the smoke + calibration cadence. Not silent residue.

## Out of scope (flagged, not designed here)

- **Phase 2 — trace flywheel → follow-up plan `docs/plans/subagent-eval-flywheel.md`**: `agent-eval-harvest.sh`, `_candidates/` staging, harvest dedup ledger, secret/PII redaction policy, candidate curation. Security-sensitive; blocked on Phase-1 calibration.
- **Coverage beyond `code-review`** (`debug-detective`, `perf-detective`, …): after the MVP proves the gate catches a real prompt regression.
- **Live merge-gate BLOCK / WARN→BLOCK graduation**: after calibration data exists.
- **Phase 3 — online / continuous eval + dashboard**: no-action. An internal fleet doesn't warrant live sampling / observability infra.
- **Product AI-assistant eval**: separate plan, pending a scope check of the `AGENTS.md` § security-review "AI-assistant" surface.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

- `98666bd0` · feat(agent-eval): Phase-1 subagent eval harness MVP (#650) — 3 schemas + pure-stdlib scorer (external judge) + runner (`--prompt-root` / `--fake-runner`) + 24 bats + 3 curated `code-review` cases + advisory wiring; incl. portable-purity fix for the rule doc.

## Deviations from plan

- **Added `tests/agent-eval/validate_schema.py`** (not in § Files to modify): a ~110-line pure-stdlib draft-07-subset JSON Schema validator. The plan's § Verification calls for "a stdlib check inside the bats, no third-party validator"; a shared committed helper is cleaner than duplicating a validator heredoc across both bats suites, and it honestly validates against the real `*-schema.json` files (resolves `$ref`, `allOf`/`if`/`then`). Used by both `agent_eval_score.bats` and `agent_eval_run.bats`.
- **Judge is invoked as `python <script>`, not `bash <script>`** (doc/test detail, not a contract change): on Windows a bare `bash` on PATH resolves to WSL when launched from the native-Python subprocess; a `python`-invoked judge runs in-interpreter. The scorer is agnostic to the judge command — this only pins the documented default + the fake judge. The judge path is converted with `cygpath -m` on Windows. Captured in `subagent-eval.md` § Windows note.
- **All `@test` names are plain ASCII** (no `→`/`≠`): bats mis-parses Unicode in test names under the default non-UTF-8 Git-Bash locale (pre-existing issue already logged in `tooling.md`, 2026-05-28). Deliberately avoided here so the suites run fully even outside a UTF-8 shell.
- **Incidental fix**: corrected 9 pre-existing broken relative links in `AGENT_SELF_IMPROVEMENT.md` (`self-improvement/categories/X` → `categories/X`) surfaced by the markdown-link gate once the file entered the diff; and reconciled 3 pre-existing stale backlog-count rows (tooling/infra/test) that the `test-backlog-counts` gate requires to match actual file counts.

## Verification (actual)

- **Scorer test (`tests/bats/agent_eval_score.bats`)**: 14/14 pass. Fake judge + fixed base/head fixtures assert the 0/1/2 exit-code contract, the delta-table shape, the `$SMATCHET_AGENT_EVAL_JUDGE_CMD` env fallback, the `--markdown-only` advisory downgrade, caseId-mismatch → exit 2, and schema conformance (sample result + all curated cases). **Passed.**
- **Runner test (`tests/bats/agent_eval_run.bats`)**: 10/10 pass. `--fake-runner` emits a `result-schema`-conformant JSON; `--prompt-root` / `--trials` / flag-parity / usage-error (exit 2) / last-line-is-path all asserted; plus a runner→scorer integration test (two fake runs score clean, exit 0). No live tokens. **Passed.**
- **Schema conformance**: the 3 curated cases validate against `case-schema.json` and a sample result against `result-schema.json` via `tests/agent-eval/validate_schema.py` (stdlib). Negative control (missing-field doc rejected) asserted. **Passed.**
- **Shell-lint**: `scripts/dev/agent-eval-run.sh` passes `agents/scripts/core/test-shell-lint.sh` (Passed: 1 Failed: 0) and direct `shellcheck` is clean. **Passed.**
- **Markdown links** (`test-markdown-links.sh --diff origin/develop`): 0 dangling links after the incidental fix. **Passed.**
- **Backlog counts** (`test-backlog-counts.sh`): Passed: 8 Failed: 0 after `--fix`. **Passed.**
- **Build gate**: N/A — no compile (tooling/docs diff).
- **Manual residue**: one live `code-review` end-to-end smoke + judge-vs-human calibration remain manual; tracked as a P2 deferred-automation entry in `docs/self-improvement/categories/tooling.md` (2026-05-31). **Not-run (deferred, tracked).**
