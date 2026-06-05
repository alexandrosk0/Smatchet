# Plan — Adopt Workflow multi-agent orchestration into the agentic infra

> **Slug**: `adopt-workflow-orchestration` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The harness exposes a `Workflow` tool — deterministic JS that fans subagents out via `agent()` / `parallel()` / `pipeline()`, with patterns (parallel-barrier, pipeline, adversarial-verify, multi-modal-sweep, loop-until-dry). A 5-dimension fit-assessment (run as a live workflow, 6 agents / 395k tokens, clean) concluded **YES_WITH_CAVEATS**: the infra is unusually well-matched — 25 markdown-prompt agents are directly callable via `agentType=<name>`, 7 are read-only (collision-free for fan-out), and `delegation.md § Parallel dispatch` already sanctions multi-agent single-block dispatch. But three real boundaries (vexp-guard Grep/Glob deny, write-set collision for parallel editors, ship-loop sequential determinism) plus an uncapped shared token budget ($76.75 logged lifetime, Opus-dominated) mean adoption must be guard-railed, not free-for-all.

Intended outcome — *after this lands, the repo has one trusted, saved Workflow (parallel pre-merge review) plus a documented decision-rule + guardrail set, so any agent knows when a Workflow is sanctioned, which agents are fan-out-safe, and how cost/collision/determinism boundaries are respected.*

Originating analysis: the in-session workflow-fit assessment (this session); no GitHub issue (process/tooling change, not a product bug → backlog-class per ADR-0014).

## Approach

Three slices, smallest-blast-radius first, all on one feature branch → one PR (one logical feature per `AGENTS.md` § PR batching).

1. **Decision-rule + guardrail doc** — add `docs/agent-rules/workflow-orchestration.md`: when a Workflow is sanctioned vs a plain `Agent`, the fan-out-safe agent roster (the 7 read-only), the three boundaries as hard rules (vexp-guard → discovery via `run_pipeline`/`get_skeleton`/shell-`rg` not Grep/Glob; parallel editors need worktree + `locks-show.sh`; ship-loop commit→push→PR→gate→merge tail stays orchestrator-owned, never Workflow-wrapped), and the cost guardrails (Opus concurrency cap 4–6, wide fan-out is sonnet/haiku read-only only, cost-estimate `AskUserQuestion` before any loop-until-dry / multi-sample-perf). Stub-link it from `AGENTS.md § Delegation` + the on-demand rule-docs table.
2. **First named workflow** — `.claude/workflows/pre-merge-review.js`: parallel-barrier `code-review` + `security-review` (both read-only → zero collision, no worktree, no lock) on a PR diff, then a judge stage collapsing the two severity-tagged punch-lists into one ranked markdown verdict. Args = PR number (or local branch diff). This is the lowest-risk highest-signal entry point and mirrors the `code-review + security-review` pair already trusted in `delegation.md`.
3. **Second named workflow (optional, deferred-eligible)** — `.claude/workflows/subsystem-invariant-audit.js`: read-only fan-out of each project specialist over its own disjoint file-zone vs its leaf `AGENTS.md`, barrier → aggregate drift report. Ship only if slice 2 lands clean; else defer with a backlog entry.

Non-obvious trade-off: `.claude/` is gitignored (per `AGENTS.md § Agent file locations`), so a workflow script committed under `.claude/workflows/` would not be tracked. Canonical home is therefore `agents/_shared/workflows/*.js` (tracked, portable), regenerated into `.claude/workflows/` by `setup-harness.sh` — confirm/extend that script during slice 2.

## Files to modify

1. `docs/agent-rules/workflow-orchestration.md` (new) — decision-rule + fan-out-safe roster + 3 boundaries + cost guardrails. Soft-warn-only size sink (~400), so room to be complete.
2. [`AGENTS.md`](../../../AGENTS.md) § Delegation + § Project rules on-demand rule-docs table — one stub line + one trigger row pointing at the new doc. Keep AGENTS.md ≤ 150 lines (navigation-only).
3. `agents/_shared/workflows/pre-merge-review.js` (new) — canonical tracked home for the first workflow (NOT `.claude/workflows/`, which is gitignored).
4. [`agents/scripts/core/setup-harness.sh`](../../../agents/scripts/core/setup-harness.sh) — confirm/extend it links `agents/_shared/workflows/*.js` → `.claude/workflows/` so the harness discovers the saved workflow. Grep first; the link rule may already cover `_shared/`.
5. `agents/_shared/workflows/subsystem-invariant-audit.js` (new, slice 3, deferral-eligible) — second workflow.
6. [`agents/README.md`](../../../agents/README.md) — add a `_shared/workflows/` row to the roster index if slices 2/3 introduce the directory.

Before adding rows: `rg -l 'workflows' agents/ .claude/ docs/` to confirm no existing workflow-script convention/dir already exists under a synonym (the assessment found none, but confirm at author time).

## Existing utilities reused

- `agentType=<name>` Workflow option — invokes the repo's own `agents/core|project/*.md` prompts as workflow subagents (no new agent code).
- The 7 read-only agents — `architect`, `code-review`, `coderabbit-triage`, `perf-detective`, `perf-measure`, `security-review`, `spike-hunter` — fan-out-safe set, reused as-is.
- `bash agents/scripts/core/locks-show.sh` (`docs/agent-rules/delegation.md`) — plan-lock collision check, the gate any *write* fan-out must pass.
- `agents/scripts/core/setup-harness.sh` — existing canonical→per-harness link generator; extend, don't reinvent.
- `.claude/.agent-tokens.jsonl` + `agents/scripts/core/agent-tokens-report.py` — existing per-workflow cost observability (SubagentStop hook); no new tracking needed.
- `delegation.md § Parallel dispatch` — the already-sanctioned multi-agent dispatch rule the new doc formalizes, not contradicts.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — agentic-infra docs + workflow scripts, zero runtime/UI code.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: no impact — no UI-thread code touched.
- **Pillar 3 (never crash)**: no impact — no product C++ touched.
- **Pillar 4 (accessibility)**: no impact — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

`N/A — pure docs + agentic-shell (`docs/agent-rules/*`, `agents/_shared/workflows/*.js`, `AGENTS.md`, `setup-harness.sh`); no `Source/Core/` / perf-gated path touched.` All five gates (PR-fast CI, Pillar-2 scanner, dispatcher drain, bucket-E, marker inventory) N/A for the same reason.

## Risks / non-goals

- **Risk: a Workflow bypasses the gated ship-loop tail.** Mitigation — the new doc makes "commit→push→PR→gate→merge stays orchestrator-owned, never Workflow-wrapped" a hard rule; workflows are scoped to diagnose/review/analyze phases only.
- **Risk: parallel-editor fan-out clobbers the shared clone.** Mitigation — doc restricts default fan-out to the 7 read-only agents; any write fan-out requires per-agent worktree + `locks-show.sh` pass (existing rules), called out explicitly.
- **Risk: vexp-guard denies Grep/Glob mid-workflow → stalled subagent.** Mitigation — doc mandates discovery via `run_pipeline`/`get_skeleton`/shell-`rg` inside workflow subagents.
- **Risk: uncapped Opus fan-out cost.** Mitigation — concurrency cap 4–6 for Opus-recruiting workflows; sonnet/haiku-only for wide sweeps; cost-estimate escalation for unbounded loops (ties to AI_POLICY.md cost-unbounded-escalate).
- **Risk: `.claude/workflows/` gitignored → lost script.** Mitigation — canonical home is tracked `agents/_shared/workflows/`, linked into `.claude/` by `setup-harness.sh`.
- **Non-goal**: wrapping the ship-loop / merge-gates / debug-detective pause-loop in a Workflow — explicitly out (those stay orchestrator-owned).
- **Non-goal**: a hard token-budget cap in `project.config.json` — governance stays human-gated per AI_POLICY.md; this plan only adds per-workflow concurrency/cost *guidance*.
- **Non-goal**: porting workflows to Codex/Cursor — the Workflow tool is Claude-Code-specific; other harnesses fall back to manual parallel dispatch (note this in the doc, don't build it).

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no C++ pure-logic changed.
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface.
- **Bash-driver scenario / screenshot / sanitizer**: N/A — no runtime behaviour.
- **Build gate**: N/A — pure-docs + agentic-shell diff (`is-pure-docs-diff.sh` / agentic-shell envelope); no `cmake --build` per § Cadence (pure-docs slices skip build + ctest).
- **Agentic-shell self-checks**: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` green (no new C++); `python agents/scripts/core/agent_size_audit.py --diff origin/develop` green (AGENTS.md ≤ 150, new doc soft-warn-only); if `setup-harness.sh` changed, run its `--selftest`/dry-run to confirm the workflow link is generated.
- **Workflow smoke**: dry-run `pre-merge-review.js` against one open PR (or the local branch diff), confirm it spawns the 2 read-only agents + judge stage and emits a single ranked table — caps concurrency at 2, touches no files, never reaches the gated tail.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script for the sub-step list). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model (Workflow-tool semantics, the `.claude/` gitignore boundary, the parallel-dispatch rule) + sharpen terms before finalising; record the outcome. Required for every plan — do not delete.
- **Manual residue**: the workflow smoke (running a real PR review) may need one human "looks right" pass on the first run; deferred-automation action = add a fixture-PR + golden punch-list assertion to a future `tests/bats` workflow check, logged in `docs/self-improvement/categories/tooling.md`. No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here (esp. slice 3 `subsystem-invariant-audit` if deferred), and revise or delete them.

- **Slice 3 (subsystem-invariant-audit workflow)** — ships only if slice 2 lands clean; otherwise deferred with a `docs/self-improvement/categories/tooling.md` backlog entry. Follow-up plan: same-shape read-only fan-out, no new infra.
- **Adversarial-verify workflow for CR "Addressed in commit X" annotations** — high-value (ties to the #780/#784 gate-escape incidents) but separate scope; no-action here, candidate for a follow-up plan once `pre-merge-review.js` proves the pattern.
- **A `tests/bats` regression harness for workflow scripts** — deferred; logged as the manual-residue follow-up above.
- **Token-budget hard cap / automated throttle** — out (governance stays human-gated per AI_POLICY.md); no-action.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
