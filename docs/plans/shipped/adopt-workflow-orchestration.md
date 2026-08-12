# Plan — Adopt Workflow multi-agent orchestration into the agentic infra

> **Slug**: `adopt-workflow-orchestration` (matches this file's basename without `.md`).
>
> **Status**: shipped — all 4 slices landed; post-ship sections populated (see § Implementation log).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The harness exposes a `Workflow` tool — deterministic JS that fans subagents out via `agent()` / `parallel()` / `pipeline()`, with patterns (parallel-barrier, pipeline, adversarial-verify, multi-modal-sweep, loop-until-dry). A 5-dimension fit-assessment (run as a live workflow, 6 agents / 395k tokens, clean) concluded **YES_WITH_CAVEATS**: the infra is unusually well-matched — 25 markdown-prompt agents are directly callable via `agentType=<name>`, 7 are read-only (collision-free for fan-out), and `delegation.md § Parallel dispatch` already sanctions multi-agent single-block dispatch. But three real boundaries (vexp-guard Grep/Glob deny, write-set collision for parallel editors, ship-loop sequential determinism) plus an uncapped shared token budget ($76.75 logged lifetime, Opus-dominated) mean adoption must be guard-railed, not free-for-all.

Intended outcome — *after this lands, the repo has one trusted, saved Workflow (parallel pre-merge review) plus a documented decision-rule + guardrail set, so any agent knows when a Workflow is sanctioned, which agents are fan-out-safe, and how cost/collision/determinism boundaries are respected.*

Originating analysis: the in-session workflow-fit assessment (this session); no GitHub issue (process/tooling change, not a product bug → backlog-class per ADR-0014).

## Approach

Four slices, smallest-blast-radius first, all on one feature branch → one PR (one logical feature per `AGENTS.md` § PR batching).

1. **Decision-rule + guardrail doc** — add `docs/agent-rules/workflow-orchestration.md`: when a Workflow is sanctioned vs a plain `Agent`, the fan-out-safe agent roster (the 7 read-only), the three boundaries as hard rules (vexp-guard → discovery via `run_pipeline`/`get_skeleton` not Grep/Glob; parallel editors need worktree + `locks-show.sh`; ship-loop commit→push→PR→gate→merge tail stays orchestrator-owned, never Workflow-wrapped), the cost guardrails (Opus concurrency cap 4–6, wide fan-out is sonnet/haiku read-only only, cost-estimate `AskUserQuestion` before any loop-until-dry / multi-sample-perf), and a short excerpt of the canonical workflow script. **Cross-link it from `docs/agent-rules/delegation.md § Parallel dispatch`, NOT from `AGENTS.md`** — AGENTS.md is 154/150 lines (over the grandfathered cap), and § Delegation already routes to delegation.md, so the chain resolves with zero AGENTS.md churn.
2. **First named workflow** — canonical `agents/_shared/workflows/pre-merge-review.js` (tracked; linked into the gitignored `.claude/workflows/` by the slice-4 setup-harness workflows loop): parallel-barrier `code-review` + `security-review` (both read-only → zero collision, no worktree, no lock), barrier, then a **judge stage** = plain `agent()` (default model, schema-forced) collapsing the two severity-tagged punch-lists into one ranked deduped verdict. **Input**: `args = {pr: N}` OR `{base: 'origin/develop'}`; no-arg default = local branch diff vs `origin/develop` (works with no GitHub remote, mirrors `/code-review`'s no-arg form); resolved target passed identically to both agents. Concurrency 2 (under the Opus cap). Lowest-risk highest-signal entry point; mirrors the trusted `code-review + security-review` pair in `delegation.md`.
3. **Second named workflow (optional, deferred-eligible)** — `agents/_shared/workflows/subsystem-invariant-audit.js`: read-only fan-out of each project specialist over its own disjoint file-zone vs its leaf `AGENTS.md`, barrier → aggregate drift report. Ship only if slice 2 lands clean; else defer with a backlog entry.
4. **Setup-harness workflows loop** — extend `agents/scripts/core/setup-harness.sh`'s `setup_claude_code` with a `link_file` loop over `agents/_shared/workflows/*.js` (modelled on the skills loop, lines 222-228) so each workflow script auto-links into the gitignored `.claude/workflows/`; add the `_shared/workflows/` row to `agents/README.md`. **Load-bearing for slice 2** — without this link the Workflow tool can't discover the saved workflow by name, so it ships in the same PR, not deferred.

Non-obvious trade-off (confirmed against the tree at grill time): `.claude/` is fully gitignored (`git ls-files .claude/` empty; `.gitignore:63`), so a script under `.claude/workflows/` is untracked. Canonical home is therefore tracked `agents/_shared/workflows/*.js`, linked into `.claude/workflows/` by `setup-harness.sh` — which today auto-links `_shared/skills/*` (lines 222-228) but has **no** workflows loop, so slice 4 ADDS one (`link_file` per script — workflows are single `.js` files, unlike skill packages). The `.js` is the single source of truth; the slice-1 doc references it + shows a short excerpt (no second full copy → no drift). Portable-purity is satisfied as long as the script embeds no new Smatchet literals (generic agent names like `code-review` are fine — verified: the gate is literals-only, baseline-gated, not harness-specificity).

## Files to modify

1. `docs/agent-rules/workflow-orchestration.md` (new, **slice 1**) — decision-rule (Workflow vs plain Agent) + fan-out-safe roster (the 7 read-only) + 3 boundaries + cost guardrails + a short excerpt of the canonical workflow script. Soft-warn-only size sink (~400), room to be complete.
2. [`docs/agent-rules/delegation.md`](../../../docs/agent-rules/delegation.md) § Parallel dispatch (**slice 1**) — add ONE cross-link line (`→ for deterministic fan-out / saved multi-agent recipes, see workflow-orchestration.md`). **No `AGENTS.md` edit** — it is 154/150 lines, over the grandfathered cap; AGENTS.md § Delegation already routes to delegation.md, so the chain resolves with zero churn.
3. `agents/_shared/workflows/pre-merge-review.js` (new, **slice 2**) — canonical tracked source of the first workflow (NOT `.claude/workflows/`, gitignored). Parallel-barrier 2 read-only reviewers → plain-`agent()` judge; `args` = `{pr}` / `{base}` / default-local-diff.
4. [`agents/scripts/core/setup-harness.sh`](../../../agents/scripts/core/setup-harness.sh) § `setup_claude_code` (**slice 4**) — ADD a workflows link loop after the skills loop (lines 222-228): for each `agents/_shared/workflows/*.js`, `link_file ".claude/workflows/<base>.js" "$wf"`. Confirmed needed — the skills loop does NOT cover workflows, and they are single files (`link_file`, not `link_dir`).
5. `agents/_shared/workflows/subsystem-invariant-audit.js` (new, **slice 3**, deferral-eligible) — second workflow.
6. [`agents/README.md`](../../../agents/README.md) (**slice 4**) — add a `_shared/workflows/` row to the roster index (the directory is introduced by slice 2's `pre-merge-review.js`).

Verified at grill time (no synonym dir exists): `agents/_shared/` holds only `skills/`, `templates/`, `token-tracking/`; no `workflows/` yet. Workflow tool discovers named workflows from `.claude/workflows/` (per the Workflow tool contract), which is why the link in #4 is load-bearing.

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

N/A — pure docs + agentic-shell (`docs/agent-rules/*`, `agents/_shared/workflows/*.js`, `AGENTS.md`, `setup-harness.sh`); no `Source/Core/` or perf-gated path touched. All five gates (PR-fast CI, Pillar-2 scanner, dispatcher drain, bucket-E, marker inventory) are N/A for the same reason.

## Risks / non-goals

- **Risk: a Workflow bypasses the gated ship-loop tail.** Mitigation — the new doc makes "commit→push→PR→gate→merge stays orchestrator-owned, never Workflow-wrapped" a hard rule; workflows are scoped to diagnose/review/analyze phases only.
- **Risk: parallel-editor fan-out clobbers the shared clone.** Mitigation — doc restricts default fan-out to the 7 read-only agents; any write fan-out requires per-agent worktree + `locks-show.sh` pass (existing rules), called out explicitly.
- **Risk: vexp-guard denies Grep/Glob mid-workflow → stalled subagent.** Mitigation — doc mandates discovery via `run_pipeline`/`get_skeleton` (semantic) inside workflow subagents; the chosen reviewers (`code-review`, `security-review`) already declare `semantic-code-search` and avoid raw Grep/Glob, so they are vexp-safe today. (`rg` is also not on PATH in the Bash tool here — semantic-first is the right default regardless.)
- **Risk: uncapped Opus fan-out cost.** Mitigation — concurrency cap 4–6 for Opus-recruiting workflows; sonnet/haiku-only for wide sweeps; cost-estimate escalation for unbounded loops (ties to AI_POLICY.md cost-unbounded-escalate).
- **Risk: `.claude/workflows/` gitignored → lost script.** Mitigation (confirmed: `git ls-files .claude/` empty) — canonical home is tracked `agents/_shared/workflows/`, linked into `.claude/workflows/` by the slice-4 `setup-harness.sh` workflows loop (modelled on the existing skills loop, lines 222-228).
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
- **Setup-harness workflows loop (slice 4)**: run `bash agents/scripts/core/setup-harness.sh claude-code` and confirm `.claude/workflows/pre-merge-review.js` link is created (idempotent re-run emits no error); the saved workflow then resolves via `Workflow({name: 'pre-merge-review'})`.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script for the sub-step list). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model (Workflow-tool semantics, the `.claude/` gitignore boundary, the parallel-dispatch rule) + sharpen terms before finalising; record the outcome. Required for every plan — do not delete. **Outcome (2026-06-05)**: verified against tree — `.claude/` fully gitignored so canonical home = `_shared/workflows/`; 7 read-only agents confirmed (real `read-only: true` frontmatter); `setup-harness.sh` has no workflows loop (slice 4 adds it); portable-purity is literals-only (CC-only `.js` is fine); CONTEXT-MAP.md has no `workflow` term (no glossary update needed; this is agentic-infra, not the domain model). 4 decisions locked: (a) cross-link via delegation.md only — AGENTS.md over the 150 cap; (b) workflow input = `{pr}` / `{base}` / default-local-diff; (c) judge = plain `agent()`, default model, schema-forced; (d) `_shared/workflows/*.js` is canonical, doc references + excerpts it.
- **Manual residue**: the workflow smoke (running a real PR review) may need one human "looks right" pass on the first run; deferred-automation action = add a fixture-PR + golden punch-list assertion to a future `tests/bats` workflow check, logged in `docs/self-improvement/categories/tooling.md`. No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here (esp. slice 3 `subsystem-invariant-audit` if deferred), and revise or delete them.

- **Slice 3 (subsystem-invariant-audit workflow)** — ships only if slice 2 lands clean; otherwise deferred with a `docs/self-improvement/categories/tooling.md` backlog entry. Follow-up plan: same-shape read-only fan-out, no new infra.
- **Adversarial-verify workflow for CR "Addressed in commit X" annotations** — high-value (ties to the #780/#784 gate-escape incidents) but separate scope; no-action here, candidate for a follow-up plan once `pre-merge-review.js` proves the pattern.
- **A `tests/bats` regression harness for workflow scripts** — deferred; logged as the manual-residue follow-up above.
- **Token-budget hard cap / automated throttle** — out (governance stays human-gated per AI_POLICY.md); no-action.

## Implementation log

Shipped as one PR (`adopt-workflow-orchestration` branch):
- Slice 1 · `docs/agent-rules/workflow-orchestration.md` (new) — decision rule (Workflow vs § Parallel dispatch vs inline), the 7 fan-out-safe read-only agents, the three boundaries (vexp-guard / write-set collision / gated-tail ownership), cost guardrails (Opus concurrency ≤ 6, sonnet/haiku for wide sweeps, escalate-before-unbounded), saved-workflow table + canonical excerpt. Cross-linked from `delegation.md` § Parallel dispatch (one line; **no AGENTS.md edit** — it is at the 154/150 grandfathered cap).
- Slice 2 · `agents/_shared/workflows/pre-merge-review.js` (new) — parallel-barrier `code-review` + `security-review` (read-only) → schema-forced judge `agent()` → one ranked deduped verdict. `args = {pr}` / `{base}` / default-local-diff.
- Slice 3 · `subsystem-invariant-audit.js` — initially **DEFERRED** (the original shape hardcoded the core-source zones, leaking a project literal into the portable `agents/_shared/` dir → `test-portable-purity` fail). **SHIPPED LATER** via runtime zone-discovery: the `.js` embeds no project literal — a discovery agent enumerates the leaf-`AGENTS.md` zones at runtime (the JS workflow runtime has no shell of its own), then one read-only `code-review` agent audits each zone vs ONLY its leaf rules → aggregated drift report. Passes `test-portable-purity`; auto-linked by `setup-harness.sh`'s `_shared/workflows/*.js` glob. See Deviations.
- Slice 4 · `setup-harness.sh` workflows `link_file` loop (after the skills loop) + `agents/README.md` `_shared/workflows/` row — auto-links every `*.js` into the gitignored `.claude/workflows/` so `Workflow({name})` resolves it.

## Deviations from plan

- **Slice 3 (subsystem-invariant-audit) initially DEFERRED, later SHIPPED** (the plan marked it deferral-eligible). The original shape audited each core-source `<ctx>/` zone vs its leaf `AGENTS.md` by hardcoding the core-source path — a project literal that leaks into the portable `agents/_shared/workflows/` dir and fails `test-portable-purity`. Resolution chosen: option (b) from the backlog — **runtime zone-discovery**. Because the Workflow JS runtime has no shell/fs/glob, the script delegates discovery to a first-phase discovery `agent()` (reads the repo's per-subsystem agent-doc registry + globs the leaf docs), then fans out one read-only `code-review` agent per zone (respecting fan-out-safety Boundary), then aggregates. The `.js` body embeds zero project literal → passes `test-portable-purity`.
- Slices 1, 2, 4 shipped in the original PR; slice 3 shipped later via the runtime-discovery shape (above).

## Verification (actual)

- **JS validity**: both `agents/_shared/workflows/*.js` pass `node --check` (async-fn-wrapped — the Workflow runtime wraps the body, so top-level `return`/`await` are legal there; a bare module-mode check falsely rejects them).
- **Slice 4 link loop**: `bash agents/scripts/core/setup-harness.sh claude-code` → exit 0, links `pre-merge-review.js` into `.claude/workflows/`; idempotent re-run exit 0.
- **Doc validation**: doc-anchors / agent-contract (27/0) / markdown-links / portable-purity / md_lint / agent-size green locally; `test-plan-ref-integrity` is green after this branch update-branches onto develop with #885's `full-function-size-compliance` ref-fix merged (a pre-existing #882 dangling ref, unrelated to this diff).
- **Build / ctest**: N/A — pure docs + agentic-shell (`is-pure-docs-diff` / agentic-shell envelope); no `Source/Core/` touched.
- **Workflow smoke**: deferred to first real use (manual residue, logged) — the saved `pre-merge-review` resolves by name post-`setup-harness`; a fixture-PR golden-punch-list bats check is the deferred-automation follow-up in `tooling.md`.
