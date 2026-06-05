# Smatchet — agent + project rules

This is the canonical entry-point doc for any agentic harness (Claude Code, Codex / OpenAI Agents, Cursor, Aider, generic). Everything an agent needs lives in this repo — no external dependencies on user-global or parent-dir config.

> **Governance layer above this contract:** [`AI_POLICY.md`](AI_POLICY.md) is the human-authority charter — humans own quality + cost, agent autonomy is a granted/revocable mode, and the **two loop modes** (`SMATCHET_LOOP_MODE`: `on` = action-biased human-on-the-loop / `in` = plan-gated human-in-the-loop, **prerelease default `in`**) plus the **escalate-when-unvalidatable** invariant bound the autonomy this file grants. `AGENTS.md` is *how* to build; `AI_POLICY.md` is *who is in control and when to stop*. The SessionStart `## === loop-mode: <on|in> ===` banner surfaces the active mode.

## Operating principles

How agents *operate* here — a skimmable map over the rules below, not new rules. Grep the principle, follow the link, skip the rest. (The Quality Pillars are quality *targets*; these 5 principles are the operating *model* — a distinct axis above them.) **Navigation only — no rule detail lives here; if a line accretes detail it has failed, move it to its linked section.**

1. **Autonomous by default** — run the ship-loop end-to-end in one turn; pause only on the defined exceptions. (§ Autonomous ship-loop default)
2. **Gate, don't trust** — every invariant is code-enforced (merge-gates, delta-lint, selftests), never a prose promise. (§ Merge gates, § Project rules § Tiered enforcement)
3. **Delegate to specialists** — the orchestrator routes to `agents/`; semantic-search before text-search. (§ Delegation, § Semantic codebase search — use it first)
4. **Plan before ship** — non-trivial work gets a plan-doc + `grill-with-docs` stress-test. (§ Process rules § Plan-doc family)
5. **Self-tighten** — every delegated agent ends with `## Self-improvement`; friction becomes prompt patches. (§ Self-improvement loop)

## Quality Pillars

Five north-star quality invariants in two sub-groups:

- **UX Pillars** (1-4) — user-facing. Pillars 1-3 are **enforceable** (auto-fail PRs that violate them); Pillar 4 is **aspirational** today (backlogged until automated checks land).
- **Engineering Pillars** (5) — code-maintainability, enforced like UX 1-3. Today: **DRY** (Pillar 5; WARN-first calibration phase per [ADR-0015](docs/adr/0015-dry-quality-pillar-duplication-gate.md)).

| # | Group | Pillar | Hard invariant | Primary owner |
|---|---|---|---|---|
| 1 | UX | Performance | Steady-state UI work ≤ **6.94 ms** (144 Hz); p99 ≤ 16.67 ms (60 Hz floor) | `perf-detective` (sustained), `spike-hunter` (p99) |
| 2 | UX | UI never freezes | No UI-thread block > 100 ms without visible cue; sync I/O on UI thread = code-review CRITICAL | `code-review`, `spike-hunter` |
| 3 | UX | Never crash | Sanitizer build clean; RAII + bounds-checked + no silent UB; graceful degradation in ship builds | `debug-detective`, `code-review`, `build-doctor` |
| 4 | UX | Accessibility | Keyboard nav, font scaling, WCAG AA contrast — flagged in backlog (no auto-fail yet) | none today (backlogged) |
| 5 | Engineering | DRY | No NEW copy-paste clone vs `origin/develop` (delta-gated `dup_audit.py`; **WARN-first**, calibration phase per ADR-0015) — copy-paste only (not structural similarity); exemptions cheap; a DRY refactor coupling independent subsystems = CRITICAL | `code-review` (reviewer-of-record + exemption sign-off) |

Visual-validation exception (Pillar 4 § Visual-validation acceptance): when no bucket-C/E coverage exists for a visual change, the orchestrator pauses the ship-loop and treats the user as the verifier — see [`docs/agent-rules/ship-loops.md`](docs/agent-rules/ship-loops.md) § Visual-validation exception.

Full enforceable-invariant text + visual-cue contract + per-pillar tooling + agent-ownership detail: [`docs/agent-rules/quality-pillars.md`](docs/agent-rules/quality-pillars.md).

## Autonomous ship-loop default

Orchestrator runs each user task end-to-end in **one turn** without pausing per stage. Default sequence:

```
diagnose → fix → build → commit → push → open PR → [gate-check] → squash-merge → git-janitor cleanup → backlog entry
```

Clarifications batched **once at start** via `AskUserQuestion`. After the user answers, the orchestrator **MUST NOT** use `AskUserQuestion` or pause for confirmation until either a defined exception fires or the post-ship 4-option menu. Each stage proceeds to the next automatically. CodeRabbit actionable findings are triaged and fixed autonomously; merge-gate polling starts immediately after PR creation; squash-merge fires immediately on `GATES_PASSED`. Loop pauses ONLY for: (1) debug-detective triggers — the pause-loop **overrides** the ship-loop (see [`docs/agent-rules/delegation.md`](docs/agent-rules/delegation.md) § Debug-mode pause-loop), (2) destructive ops outside scope, (3) cross-repo / external-service mutations, (4) anything not durably authorised, (5) **visual-validation exception** (touches `SmatchetTheme.cpp` / `Smatchet*Ui*.cpp` / `Locales/*.json` / `ImVec4` constants AND no bucket-C/E coverage — pause after build with launched exe, await user verdict), (6) **cannot-autonomously-validate / cost-unbounded — escalate** (per [`AI_POLICY.md`](AI_POLICY.md) § Escalate, don't assume: no gate/test/spec confirms correctness, scope unauthorised, or spend is unbounded → stop and `AskUserQuestion` with the blocker named, never guess; fires in **both** loop modes). In **human-in-the-loop** mode (prerelease default) the loop additionally pauses at any decision **not covered by the approved plan** — see [`AI_POLICY.md`](AI_POLICY.md) § Two loop modes.

`SMATCHET_AGENT_VCS=p4` flips the loop to the **P4-gated** variant: smoke build → shelve → user review in P4V → full tests → submit → git branch + push + PR. Git is touched **once**, at the end, after shelf approval AND test-pass. Sub-variants chosen via `AskUserQuestion`: small-change loop (single slice, `//smatchet/main`) or task-stream loop (multi-slice, `agents/scripts/project/p4-task-stream.sh`).

**Session-start self-check (mandatory, regardless of user-prompt flavour)**. The SessionStart hook (`agents/scripts/core/clear-session-context.sh`) emits a `## === p4-mode ACTIVE ===` banner into `.session-context.md` when `$SMATCHET_AGENT_VCS=p4` AND `p4 info` succeeds. When the orchestrator sees that banner, it MUST follow the P4-gated ship-loop for ALL subsequent task-loops in this session — even when the user's prompt is git-flavoured (PR numbers, gh URLs, etc.). The env-var opt-in overrides prompt-driven mode inference. On `p4 info` failure the banner reads `## === p4-mode REQUESTED but UNREACHABLE ===` and the orchestrator routes through `AskUserQuestion` per `docs/agent-rules/ship-loops.md` § P4-gated ship-loop (never silently downgrade). `agents/scripts/project/p4-git-sync-check.sh` checks git-pending vs `p4 opened` alignment and is part of the P4-mode pre-ship verification.

After the loop completes, the orchestrator emits the **post-ship 4-option `AskUserQuestion`**: Manual verify / Review PR / Register with watcher (auto-merges when gates pass) / Done. Skip-condition: if the user already said "merge when green", enter option 3 directly.

**PR batching — one PR per logical feature, not one per slice/task.** Related slices that serve a single coherent goal accumulate on **one** feature branch and ship as a **single** PR; `open PR` is reached once per logical feature, not after each stage. This respects both CodeRabbit limits at once — the review **quota** (N tiny PRs burn N reviews for one conceptual change) and the per-PR **file ceiling** (above it CR posts *review-skipped — too many files* and the gate blocks). Cohesion is the seam: unrelated work in subsystem A vs B = two PRs; if one feature's cumulative diff would exceed the file ceiling, split along natural seams (don't batch past it expecting `cr-out-of-band` to cover routine batching). Earlier slices of a feature stop at `commit`/`push` to the shared branch and defer PR creation until the feature is whole. The do-not-pause checklist + merge gates + post-ship menu fire **per PR**, so one feature-PR may cover several session tasks.

Full sequence + per-exception detail + P4-gated phases + post-ship protocol + PR-batching boundary rules: [`docs/agent-rules/ship-loops.md`](docs/agent-rules/ship-loops.md).

## Merge gates

Before any squash-merge by the orchestrator, `git-janitor`, or `smatchet-merge-watcher`, the gate-poller (`agents/scripts/core/merge-gates.sh` + `agents/scripts/core/merge-gates.graphql`) checks three conditions via one `gh api graphql` call:

1. **CI** — every required check on the head commit reaches a passing terminal state (CheckRun: `conclusion ∈ {SUCCESS, NEUTRAL, SKIPPED, STALE}`; StatusContext: `state == SUCCESS`).
2. **CodeRabbit** — `APPROVED` or `COMMENTED + Actionable comments posted: 0` passes; `COMMENTED + N > 0`, `CHANGES_REQUESTED`, `DISMISSED`, `STALE_WITH_FINDINGS`, `STALE_UNKNOWN` block. `NONE` falls through after `MERGE_GATES_CR_GRACE_POLLS` (default 10) expires when CR is installed — but the poller now also auto-posts `@coderabbitai review` once per HEAD on the first blocking `NONE` poll (early-nudge, gated by `MERGE_GATES_STALE_REREVIEW_POLLS`; 0 disables), so CR usually resolves before the grace backstop. **Review-skipped (too many files)** — when CR posts a PR conversation comment saying it skipped review because the PR exceeds its file limit (marker `skip review by coderabbit.ai`; `reviewDecision` stays `NONE`), the gate **blocks** (overriding the NONE grace fall-through, which would otherwise merge a huge unreviewed PR) and suppresses the futile auto-nudge; remediation is to split the PR or apply `cr-out-of-band`. A genuine on-head review always wins over a stale skip comment. The `cr-out-of-band` PR label downgrades any CR block (state verdict, unresolved CR-authored threads, **or** a review-skipped comment) to WARN — CR gate only; CI + user-comment gates still bind.
3. **User comments** — zero unresolved non-outdated review threads from non-bot non-self authors; zero conversation-tab comments from same.

Plus: PR is OPEN, `reviewDecision ∈ {APPROVED, null}`, no GraphQL `hasNextPage` overflow. **Auto-merge applies only when explicitly authorised** (post-ship "Register with watcher" or in-session "merge when green"). When authorised, the caller sets `MERGE_GATES_FLIP_READY=true` so the poller flips draft→ready at start, letting CodeRabbit's `auto_review.drafts:false` config not bypass review on draft PRs (closes the C4 draft-PR bypass per ADR 0006 amendment). Manual CR re-review trigger: `gh pr comment <pr> --body "@coderabbitai review"`. `SKIP_MERGE_GATES=true` at session init bypasses globally; per-PR label overrides (`tests-out-of-band`, `perf-out-of-band`) downgrade specific failing CI checks to WARN, and `cr-out-of-band` downgrades a CodeRabbit block to WARN.

Halt prompts on block / timeout / API-error / closed-externally / pagination overflow route through `AskUserQuestion` with explicit return-code-keyed options. The REST squash-merge contract (`gh api -X PUT repos/.../pulls/N/merge -f merge_method=squash`) is the merge mechanism; conflicts + branch-protection are enforced by GitHub, not duplicated locally.

**Never merge past ANY red check — required or not.** GitHub's "required check" set only governs what *blocks* a merge; a **non-required** check that is RED is still a real failure, never a fake one. Before any squash-merge — and **especially** a direct `gh api -X PUT … /merge` (which bypasses the gate-poller entirely) — every check on the head must be terminal-green (`SUCCESS`/`NEUTRAL`/`SKIPPED`/`STALE`), not just the four required ones. A red non-required job (e.g. **"Doc anchors + agent contract"**, which runs the whole `test-docs.sh` doc-validation suite — `test-portable-purity` / `test-plan-index` / `test-plan-ref-integrity`) means real breakage will land on `develop`. The **only** exceptions are (a) an explicit per-PR override label (`tests-out-of-band` / `perf-out-of-band` / `cr-out-of-band`) that *names* the downgraded check, or (b) a check you have *positively confirmed* is an irrelevant flake (named, with the reason). A red check is a stop; "non-required" is not a reason to ignore it. Direct admin-merge is for breaking a **stale-BLOCKED** state where every check is actually green — not for forcing past a genuinely red job. (Incident: this discipline was violated twice in one session — #780 admin-merged past a red CodeRabbit finding, #784 past a red doc-validation job — each shipping breakage that needed a follow-up heal. Captured in [`docs/self-improvement/postmortems.md`](docs/self-improvement/postmortems.md).)

**Never trust a CodeRabbit "✅ Addressed in commit X" annotation blindly** — CR matches commit-message keywords, not the diff. When CR marks one of its own findings addressed, read the cited commit's diff against the original finding and confirm the change actually matches the requested fix before treating it resolved (observed: a "✅ Addressed" banner-contrast fix that the cited commit never made). The green annotation is a hint, not proof.

Full per-outcome semantics + halt-prompt return-code table + env-knob list + REST contract: [`docs/agent-rules/merge-gates.md`](docs/agent-rules/merge-gates.md). Tests: `tests/bats/merge_gates.bats`.

## Issue triage

**GitHub Issues are canonical for product bugs** ([ADR-0014](docs/adr/0014-github-issues-canonical-for-product-bugs.md)); the internal `docs/self-improvement/categories/*` backlog is for the **agent system itself** (process/tooling/infra/test/security) **+ product tech-debt** (the `debt` category). The old `bug` category is **deprecated**. The bug-vs-debt rule (keyed on observable effect): user-observable defect or correctness/safety violation → **GitHub Issue**; internal maintainability with no observable defect → **`debt.md`**; ambiguous → leave in backlog + flag a human. On a confirmed product bug the orchestrator dedup-greps open Issues and (if none) `gh issue create`s a structured, labelled Issue instead of appending to `bug.md`. CR auto-Issues + stale/dup Issues are reconciled by `issue-sweep.sh` (closeout) + the `issue-janitor` (periodic) — auto-acting only on **bot**-authored strays, never auto-closing a **human** Issue. **Fixing** an Issue is **user-initiated + label-routed**: `gh issue develop <n>` → the `area:<subsystem>` label names the specialist → ship-loop with `Fixes #<n>` auto-closes it; the closeout sweep **auto-proposes** the top `P0`/`P1` (`[issue-propose]`) but **never auto-fixes** product code. Full protocol + boundary table + decision tree + elevation flow + labels: [`docs/agent-rules/issue-triage.md`](docs/agent-rules/issue-triage.md).

## Project rules

**Doc & agentic structure**: the normative taxonomy + the portable/project boundary live in [`docs/STRUCTURE.md`](docs/STRUCTURE.md); project-specific values (build presets, perf budgets, lint zones, env-prefix, …) live in [`project.config.json`](project.config.json). Portable dirs (`agents/core/`, `agents/_shared/`, `docs/agent-rules/`, `docs/harness/`) read values from the config — don't hardcode (guard: `test-portable-purity`).

**Every-edit invariants** (all first-party C++; full mechanics → [`cpp-rules.md`](docs/agent-rules/cpp-rules.md), building → [`build.md`](docs/agent-rules/build.md)):

- **Language**: C++14 hard (Unreal compat). Banned: `string_view`, `optional`, `variant`, structured bindings, `if constexpr`. Must compile on MSVC + Clang.
- **Logging**: `LOG_{DEBUG,INFO,WARN,ERROR,TRACE}` from `Logger.h` — never `printf` / `std::cerr`. Named exceptions (matching inline comment): `Source/Standalone/` pre-logger-init (`// pre-logger-init — LOG_* unavailable`), CLI stdout (`// CLI stdout — product output, not logging`).
- **RAII**: no raw `new`/`delete` — `std::unique_ptr` + `make_unique` (markers `// C-ABI handle`, `// custom-deleter`); `const&` for non-trivial params; `std::move` on last use. (A `std::unique_ptr<T>` member in a header needs `T`'s full definition included there — see [`cpp-rules.md`](docs/agent-rules/cpp-rules.md) § Quality.)
- **Exceptions**: empty `catch (...) {}` = review CRITICAL ([`exception-handling-policy.md`](docs/agent-rules/exception-handling-policy.md)).
- **Don't**: add GLFW/OpenGL includes to `Source/Core/` headers (DX12 compiles them too); redefine `IMGUI_USE_WCHAR32` (PUBLIC on `ImGuiLib`); use `obj = {...}` brace-list reassignment for nlohmann (`obj["k"] = v` instead).

**Prompt/contract size**: agent prompts (`agents/core/*.md` + `agents/project/*.md`) stay ≤ **250 lines** (soft-warn 150); `AGENTS.md` stays ≤ **150 lines** (navigation-only; soft-warn 120); the extraction *sinks* (`docs/agent-rules/*.md` + `agents/_shared/skills/**`) are **soft-warn-only** (≈400, never block — a hard cap there would fight the extraction they receive). Delta-gated by `agent_size_audit.py --diff origin/develop` (rule id `agent-too-long`); existing over-cap files are grandfathered ([`docs/high-integrity/agent-size-baseline.md`](docs/high-integrity/agent-size-baseline.md)) — only NEW files over cap or a file crossing its cap fail; `SMATCHET_DEVIATION(rule=agent-too-long; …)` anywhere in the file escapes. Shrink the whales by extracting agent procedure-bodies to skills + `AGENTS.md` rule-detail to `docs/agent-rules/*` per [`docs/agent-rules/AGENT-VS-SKILL.md`](docs/agent-rules/AGENT-VS-SKILL.md).

**Enforcement contract-card** — the gated rule-ids + zones + caps (single source of truth; each gate's `--selftest` asserts these tokens live here; full mechanics → [`cpp-rules.md`](docs/agent-rules/cpp-rules.md) § Tiered enforcement). Run all gates locally before every push: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` (or `bash scripts/dev/pre-ship.sh`). Delta-gated vs `origin/develop`, existing violators grandfathered, `SMATCHET_DEVIATION(rule=<id>; reason=…; owner=…; revisit=…)` escapes.

| Rule | Scope | Cap / note |
|---|---|---|
| `no-printf-stderr`, `define-imgui` | strict zone | grep rules |
| `no-raw-new`, `deviation-overdue`, `no-detach` | all first-party C++ | absolute (0; no grandfathering) |
| `comment-commented-out-code`, `comment-decorative-banner`, `comment-blank-run` | all first-party C++ | comment-regrowth |
| `function-too-long` | all first-party C++ | **120** lines non-UI / **200** ImGui-draw (path under `Ui/` OR name starts `Draw`/`Render`) |
| `function-too-branchy` | all first-party C++ | **30** decision points |
| `agent-too-long` (`agent_size_audit.py`) | agent prompts / `AGENTS.md` | **250** / **150** lines |
| `duplication` | all first-party C++ | copy-paste clone (WARN-first calibration) |
| `narrowing-conversions` | strict zone (Windows post-merge job) | clang-tidy |

**Strict zones** (any violation fails): `Source/Core/src/Tracker/`, `Source/Core/src/Sync/`, `Source/Core/src/Persistence/`, `Source/Core/src/Config/`, `Source/Core/src/Commands/`, `Source/Plugins/Mcp/` (+ matching `Source/Core/include/`). **Light** (ungated): `Source/Core/src/Ui/`, `Source/Standalone/`. **Exempt**: `ThirdParty/`, `build/`.

**On-demand rule-docs** — load the one the task fires; don't carry them otherwise:

| Trigger | Doc |
|---|---|
| building (presets · light build · warnings-as-errors · MSYS2-retired · Unreal-lib clearing · dual-target verify) | [`build.md`](docs/agent-rules/build.md) |
| editing C++ (layout · libs · quality · file-split · ImGui-draw · **tiered-enforcement** mechanics · `SMATCHET_DEVIATION` grammar · lint hook · shell-lint · subagent-eval · subsystem leaf-docs) | [`cpp-rules.md`](docs/agent-rules/cpp-rules.md) |
| debugging (pink-clear · exe-staleness) | [`debug-techniques.md`](docs/agent-rules/debug-techniques.md) |
| optimizing / FPS / lag / hitch | [`docs/guides/perf-workflow.md`](docs/guides/perf-workflow.md) |
| golden artefact (`tests/golden/*`, snapshots) | [`golden-image-approval.md`](docs/agent-rules/golden-image-approval.md) |
| handling an issue | [`issue-triage.md`](docs/agent-rules/issue-triage.md) (also § Issue triage) |
| p4-mode | [`docs/perforce/AGENT_FLOWS.md`](docs/perforce/AGENT_FLOWS.md) |

**Subsystem guides**: when you touch `Source/Core/src/<ctx>/`, read its leaf `AGENTS.md` first — single source of truth, overrides any central summary. Registry: [`CONTEXT-MAP.md`](CONTEXT-MAP.md) (detail in [`cpp-rules.md`](docs/agent-rules/cpp-rules.md) § Subsystem guides).

## Process rules

Rules for **how agents move work through the pipeline** — plan-doc lifecycle, destructive-VCS-op discipline, cadence + verification, and the meta-rule for where future rules land. Companion files: [`docs/agent-rules/merge-gates.md`](docs/agent-rules/merge-gates.md), [`docs/agent-rules/ship-loops.md`](docs/agent-rules/ship-loops.md), [`docs/agent-rules/delegation.md`](docs/agent-rules/delegation.md).

- **Plan-doc family** — every plan lives at `docs/plans/active/<slug>.md`, committed immediately with `wip(plan): <slug>` (working-tree-only files are silently lost on checkout). Post-ship § Implementation log + § Deviations + § Verification update. Stress-test via `grill-with-docs` skill before finalising. Start from `docs/plans/active/_plan-template.md`. **Perf-gate section mandatory** when diff touches `Source/Core/`. **Scope-reduction edits + final-check grep**: when a plan is slimmed by deferring a feature, grep `**/CONTEXT*.md` (covers `CONTEXT.md`, `CONTEXT-MAP.md`, per-context `src/<ctx>/CONTEXT.md`), `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for **every symbol named in the now-deferred work** and revise or delete hits that describe deferred-as-current truth. Same grep after any substrate/shape/contract rewrite — hit the keyword family of the changed concept and clear every stray reference. **Plan-revision direct-pushes are PR-only**: revisions to `docs/plans/active/<slug>.md` § Implementation log / § Deviations / § Verification (actual) ship via a follow-up PR (same PR if still open, follow-up PR if already merged) — never direct-push to develop, even for one-line edits. Eliminates classifier-vs-rule drift.
- **Git/p4 discipline** — 5-step pre-flight before any destructive git op (`reset --hard`, `clean -f`, `branch -D`) against a worktree this session didn't personally check out: branch-show, status-short, stash modified, op, decide-stash-fate. Same defensive principle for destructive p4 verbs in p4-mode. Force-push banned globally except a narrow `--force-with-lease` carve-out on `claude/<id>/*` and `agent/<task-stream-id>/*` branches during API-500 recovery (ahead-range zero non-self commits; never bare `--force`). **Worktree-absolute path discipline**: when the session's working directory is under `.claude/worktrees/<id>/`, all `Edit` / `Write` absolute paths MUST use the worktree prefix — not the main-repo prefix. Main-repo-prefixed paths land changes on whatever branch main currently has checked out (often a sibling agent's branch), causing cross-branch contamination. Verify via `git rev-parse --show-toplevel` if uncertain.
- **Cadence + verification** — `cmake --build` and `scripts/dev/test-all.sh` run **at most once per slice**, after implementation is complete. Pure-docs slices skip both (`agents/scripts/core/is-pure-docs-diff.sh`). Trivial-visual envelope skips bucket-E + isolated worktree. Perf scenario auto-runs at slice boundary when the diff hits the curated map. **Stale-read recovery on `Edit`** = Re-Read → diff intended change → Re-Edit (never `replace_all` as force-write). Deferred lint drains once at end-of-turn; `clang-format -i` still runs inline.
- **Deferred plan-file rows** — optional/skipped § Files to modify rows require same-turn `## Deviations from plan` + backlog entry when follow-up work remains (`docs/agent-rules/process-rules.md`).
- **Memory drain** — the harness auto-memory inbox (`~/.claude/projects/<slug>/memory/`) is transient, not durable. A `SessionStart` nudge (`agents/scripts/core/memory-drain-nudge.sh`) fires when it holds ≥ 5 items or any item is > 7 days old; act via the `/drain-memory` skill (or "drain memory"). Triage every item: **implement** (→ `AGENTS.md` / `docs/agent-rules/*`), **backlog** (→ `docs/self-improvement/categories/*`), or **toss** — verifying each claim against the current tree first, then clearing drained rows. A cloud routine can't drain it (dir is machine-local). Spec: [`docs/agent-rules/memory-drain.md`](docs/agent-rules/memory-drain.md).
- **Where new rules go** — 1-liners stay in § Project rules above; rules that fit an extracted topic land in that file; > 30-line new topics get their own `docs/agent-rules/<topic>.md` + stub here; ≤ 30-line orphans go in `process-rules.md` (the catch-all) rather than fragmenting.

Full sub-rule text + canonical recipes + carve-out exclusion list + hot-files list + the deferred-lint pipeline: [`docs/agent-rules/process-rules.md`](docs/agent-rules/process-rules.md).

## Debug techniques

Pink-clear UI gap detection + the exe-staleness check → [`docs/agent-rules/debug-techniques.md`](docs/agent-rules/debug-techniques.md) (project-wide, mandatory whenever they apply). The full behavioural-bug investigation loop is `agents/core/debug-detective.md` + its `debug-instrument` skill.

## Semantic codebase search — use it first

Every agent in this repo expects the orchestrator (or the agent itself) to use **semantic codebase search** before falling back to text-search. In practice that means:

- **Always** call the harness's indexed codebase search first for any "where is X" / "what calls Y" / "what does this touch" question. This is faster, cheaper, and more accurate than raw `grep` over a multi-MLOC codebase.
- Prefer **compact file-skeleton views** (signatures + classes only) for files you're inspecting but not editing — typically 70–90% token savings vs full reads.
- Fall back to text-search + full reads only when no semantic search is available or its index is degraded.

Under Claude Code this maps to `mcp__vexp__run_pipeline` (semantic search) and `mcp__vexp__get_skeleton` (skeleton). Other harnesses substitute their equivalents (see § Harness adapter → [`docs/harness/capability-adapter.md`](docs/harness/capability-adapter.md)). Agents whose prose mentions vexp do so as a concrete example — the capability is what matters.

## Agent file locations

Canonical, single source of truth: `agents/<name>.md` at the repo root (per the [agents.md spec](https://agents.md/)). Shared scripts + skills live at `agents/_shared/`. **Agent vs skill** — which form a new recurring procedure should take is decided by the rubric in [`docs/agent-rules/AGENT-VS-SKILL.md`](docs/agent-rules/AGENT-VS-SKILL.md) (skill when all-bounded-deterministic-inline; agent when any-exploration-loop-spawn-delegates; dual-publish to keep cross-harness discovery).

Per-harness adapter directories (`.claude/`, `.codex/`, `.cursor/`) are **gitignored** — they're regenerated locally from the canonical tree by `bash agents/scripts/core/setup-harness.sh <name>`. Adapters use directory junctions / symlinks where possible so edits to `agents/*.md` are picked up by the harness immediately, no sync step.

First-time setup or fresh clone? See [`docs/harness/SETUP.md`](docs/harness/SETUP.md).

## Delegation

**Moved to** [`docs/agent-rules/delegation.md`](docs/agent-rules/delegation.md) (~230 lines lifted out for navigability — AGENTS.md is now ~320 lines instead of 549).

Default: stay in the orchestrator's primary model for routine work. Delegate to an agent in `agents/` when the task matches.

Quick index of moved subsections — full content in `docs/agent-rules/delegation.md`:

- **Orchestrator delegation packet** — plan-lock pre-flight, shared inventory, invariant decisions, output budget, plan revision contract, subagent progress markers reminder, pure-helper TU-split recipe.
- **Parallel dispatch** — when to run multiple subagents in one tool-use block.
- **Context budget by task class** — ~80% utilization ceiling for large refactors → delegate before the last 20%; high tolerance for low-sensitivity edits.
- **Session scratchpad protocol** — `.session-context.md` lifecycle + `## Session context append` shape.
- **Subagent progress markers** — `.progress.log` via `bash agents/scripts/core/agent-progress.sh`.
- **Tool-trace contract** — hook-derived; agents don't track manually.
- **Agent output contract** — 5-class table (Investigator / Diagnostic read-edit / Implementer / Helper / Maintenance) + `## Outcome:` mandate.
- **Trigger auto-activation** — keyword → agent routing table.
- **Debug-mode pause-loop (overrides ship-loop)** — for `debug-detective` triggers.
- **API-500 mid-run recovery** — 5-step recovery for delegated agents that error API-500 after shipping file edits; `git add -A` gotcha; force-push carve-out for spawned-agent branches.
- **Skeleton-first** — `get_skeleton` for inspection, `Read` for editing.
- **Agent versioning** — when to bump `version: <N>`.
- **Cross-cutting** + **Subsystem specialists** — delegation tables.
- **Stay in the orchestrator for** — routine work list.
- **Heuristic** — when to delegate vs handle directly.
- **`delegates-to:` frontmatter** — direct call vs orchestrator-routed.
- **Why split** + **Complexity rationale** — design intent.

External references to `AGENTS.md § <subsection>` continue to resolve via this index — agents who read AGENTS.md land here, see the cross-link, and follow it to the canonical text. Don't maintain parallel copies; edit the canonical at `docs/agent-rules/delegation.md`.

## Self-improvement loop

Every delegated agent ends its report with a `## Self-improvement` section. **Empty is the common case and explicitly fine** — agents only flag real friction, never make up suggestions.

Operational rules — format, categories (`bug` / `process` / `tooling` / `infra` / `test` / `security`), priority enum (P0–P3), workflow steps, apply threshold, triage cadence — live alongside the index at [`docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`](docs/self-improvement/AGENT_SELF_IMPROVEMENT.md). Live entries split per category under [`docs/self-improvement/categories/`](docs/self-improvement/categories/). Applied entries archive immediately to `self-improvement/categories/applied.md`. The goal is a self-tightening loop — agents notice friction, the orchestrator accumulates evidence, prompts get patched, friction drops.

**Gate escapes owe a postmortem.** A gate escape — something that shipped to `develop` that a gate should have caught (a non-SUCCESS check at merge, an override label, a `Revert`, an overdue deviation) — is the highest-signal lesson for a "gate, don't trust" harness. `agents/scripts/core/postmortem-owed.sh` detects them and raises a SessionStart nudge; the [`gate-escape-postmortem`](agents/_shared/skills/gate-escape-postmortem/SKILL.md) skill runs a blameless RCA whose **mandatory** `### Preventing gate` field names a new gate (filed into the categories above via the normal threshold) and appends to the [`docs/self-improvement/postmortems.md`](docs/self-improvement/postmortems.md) ledger. The postmortem is the incident *finder*; this loop is the *applier*.

## Dual-VCS topology (Perforce as opt-in local layer)

Smatchet runs git/GitHub as the **ship-line** (PR review, CI, `smatchet-merge-watcher`) and Perforce as an **opt-in local layer** (`SMATCHET_AGENT_VCS=p4`; default `git`) for agentic-WIP primitives — named server-side shelves, atomic counters as plan-locks, exclusive `+l` file locks, task streams as parallel-isolation primitives. The Perforce layer is purely additive: never required, never authoritative, never on the ship-line.

Concern-by-concern git ↔ p4 mapping + verb-level TL;DR + lock discipline + shelf-vs-stash + destructive-p4-op pre-flight: [`docs/perforce/AGENT_FLOWS.md`](docs/perforce/AGENT_FLOWS.md). Bring-up: [`docs/perforce/SETUP.md`](docs/perforce/SETUP.md). Janitor: [`agents/core/p4-janitor.md`](agents/core/p4-janitor.md). Plan: [`docs/plans/shipped/git-to-perforce-migration.md`](docs/plans/shipped/git-to-perforce-migration.md).

## Harness adapter

Each agent declares a closed set of **capability tags**; the orchestrator (and the harness) maps tags to concrete tools. The full capability-tag → per-harness tool table + the per-harness discovery notes live in [`docs/harness/capability-adapter.md`](docs/harness/capability-adapter.md) (load when porting to a new harness or resolving what a tag means here).

**Per-agent harness hints**: each agent's YAML frontmatter may carry a `harness-hints.<harness>:` block with harness-specific routing details (e.g. Anthropic model selection, MCP tool list). Harnesses ignore unknown blocks.

## Recommended companion — caveman

Output-token compressor (~75% cut, technical content preserved byte-for-byte). Install + use instructions: [`docs/guides/caveman.md`](docs/guides/caveman.md). Default: `/caveman full` at session start.

## Semantic-search exceptions

- **Exhaustive literal / symbol inventories**: use text-search (`rg` / harness equivalent), not semantic search. Graph-ranked results are not exhaustive. Run the search once in the orchestrator and pass `<file>:<line>:<role>` matches inline to delegated agents.
- **Mechanical renames and cleanup checks**: same — every occurrence must be found. `mechanic` and `perf-instrument` already use text-search per their prompts.
- **Understanding impact / ownership / surrounding logic**: semantic search stays primary. This is the default path.

## vexp — Claude-Code-only

The vexp MCP-tool guidance block (`run_pipeline`, `get_skeleton`, etc.) is Claude-Code-specific and lives in `.claude/CLAUDE.md` (regenerated by the vexp tool; sourced from `docs/harness/claude-code/CLAUDE.md.tmpl`). It is deliberately **not** mirrored here so Codex / Cursor / Aider — which read `AGENTS.md` per the [agents.md spec](https://agents.md/) — don't carry Claude-Code-only MCP guidance they cannot use. Those harnesses fall back to text-search per § Harness adapter.

The vexp tool currently auto-regenerates its block into `AGENTS.md` on every install / update, which is wrong per the above rationale. `agents/scripts/core/vexp-strip-agents-md.sh` (wired as a SessionStart hook in `.claude/settings.json`) idempotently strips the block on every Claude Code session start until the upstream tool is patched to target `.claude/CLAUDE.md` only. Tracked: `docs/plans/shipped/unblock-external-blockers-2-3-4.md` § Slice 2.
