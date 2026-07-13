# Smatchet — agent + project rules

This is the canonical entry-point doc for any agentic harness (Claude Code, Codex / OpenAI Agents, Cursor, Aider, generic). Everything an agent needs lives in this repo — no external dependencies on user-global or parent-dir config.

> **Governance layer above this contract:** [`AI_POLICY.md`](AI_POLICY.md) is the human-authority charter — humans own quality + cost, agent autonomy is a granted/revocable mode, and the **two loop modes** (`SMATCHET_LOOP_MODE`: `on` = action-biased human-on-the-loop / `in` = plan-gated human-in-the-loop, **default in `project.config.json` § governance**) plus the **escalate-when-unvalidatable** invariant bound the autonomy this file grants. `AGENTS.md` is *how* to build; `AI_POLICY.md` is *who is in control and when to stop*. The SessionStart `## === loop-mode: <on|in> ===` banner surfaces the active mode.

## Operating principles

Skimmable map (operating *model*; the Quality Pillars are the quality *targets*). **Navigation only — no rule detail here; if a line accretes detail, move it to its linked section.** **1** Autonomous by default (§ Autonomous ship-loop default) · **2** Gate, don't trust (§ Merge gates) · **3** Delegate to specialists (§ Delegation) · **4** Plan before ship (§ Process rules) · **5** Self-tighten (§ Self-improvement loop).

## Quality Pillars

Five north-star invariants:
- **UX Pillars** 1-4 (user-facing; 1-3 enforceable / auto-fail PRs, 4 aspirational-backlogged) + **Engineering Pillar 5 — DRY** (blocking delta-gate, graduated from WARN-first 2026-06-21, [ADR-0015](docs/adr/0015-dry-quality-pillar-duplication-gate.md)):

| # | Group | Pillar | Hard invariant | Primary owner |
|---|---|---|---|---|
| 1 | UX | Performance | Steady-state UI work ≤ **6.94 ms** (144 Hz); p99 ≤ 10.0 ms (100 Hz floor) | `perf-detective` (sustained), `spike-hunter` (p99) |
| 2 | UX | UI never freezes | No UI-thread block > 100 ms without visible cue; sync I/O on UI thread = code-review CRITICAL | `code-review`, `spike-hunter` |
| 3 | UX | Never crash | Sanitizer build clean; RAII + bounds-checked + no silent UB; graceful degradation in ship builds | `debug-detective`, `code-review`, `build-doctor` |
| 4 | UX | Accessibility | Keyboard nav, font scaling, WCAG AA contrast — flagged in backlog (no auto-fail yet) | none today (backlogged) |
| 5 | Engineering | DRY | No NEW copy-paste clone vs `origin/develop` (delta-gated `dup_audit.py`; **blocking** — graduated from WARN-first 2026-06-21, ADR-0015) — copy-paste only (not structural similarity); exemptions cheap (`SMATCHET_DEVIATION(rule=duplication)`); a DRY refactor coupling independent subsystems = CRITICAL | `code-review` (reviewer-of-record + exemption sign-off) |

Visual-validation exception (Pillar 4): no bucket-C/E coverage for a visual change → the orchestrator pauses and the user verifies ([`ship-loops.md`](docs/agent-rules/ship-loops.md) § Visual-validation exception). Full enforceable-invariant text + visual-cue contract + per-pillar tooling: [`docs/agent-rules/quality-pillars.md`](docs/agent-rules/quality-pillars.md).

## Autonomous ship-loop default

Orchestrator runs each user task end-to-end in **one turn** without pausing per stage. Default sequence:

```
diagnose → [seed plan-lock] → fix → build → commit → push → open PR → [gate-check] → squash-merge → git-janitor cleanup → backlog entry
```

Clarifications batched **once at start** via `AskUserQuestion`; after that the orchestrator **MUST NOT** pause until a defined exception or the post-ship menu (CR findings triaged autonomously; merge-gate polling starts at PR creation; squash-merge fires on `GATES_PASSED`). Pause ONLY for: (1) debug-detective triggers (pause-loop **overrides** ship-loop, [`delegation.md`](docs/agent-rules/delegation.md) § Debug-mode pause-loop), (2) destructive ops outside scope, (3) cross-repo / external-service mutations, (4) anything not durably authorised, (5) **visual-validation exception** (touches `SmatchetTheme.cpp` / `Smatchet*Ui*.cpp` / `SmatchetLocalization.cpp` / `ImVec4` + no bucket-C/E coverage → pause with launched exe, await verdict), (6) **cannot-validate / cost-unbounded → escalate** ([`AI_POLICY.md`](AI_POLICY.md) § Escalate, don't assume; **both** loop modes). In **human-in-the-loop** mode also pause at any decision **not covered by the approved plan** ([`AI_POLICY.md`](AI_POLICY.md) § Two loop modes).

After the loop, emit the **post-ship 4-option `AskUserQuestion`** (Manual verify / Review PR / Register with watcher / Done; skip to option 3 if the user said "merge when green") — **UNLESS `governance.auto_merge: on` is active**, which skips the prompt and auto squash-merges on a passing gate-poll (§ Merge gates). **PR batching**: one PR per logical *feature*, not per slice — related slices accumulate on one branch and ship once (respects CodeRabbit's review quota + per-PR file ceiling; split along seams if the diff would exceed the ceiling). `SMATCHET_AGENT_VCS=p4` flips to the **P4-gated** variant (shelve → P4V review → submit → git at the end); the mandatory p4-mode session-start self-check + phases + sub-variants are in [`docs/agent-rules/ship-loops.md`](docs/agent-rules/ship-loops.md) § P4-gated ship-loop + [`docs/perforce/AGENT_FLOWS.md`](docs/perforce/AGENT_FLOWS.md). Full sequence + per-exception detail + post-ship protocol + PR-batching rules: [`docs/agent-rules/ship-loops.md`](docs/agent-rules/ship-loops.md).

## Merge gates

Before any squash-merge (orchestrator / `git-janitor` / `smatchet-merge-watcher`), the gate-poller (`agents/scripts/core/merge-gates.sh`) checks four conditions via one `gh api graphql` call: **(1) CI** — **block-on-any-red**: EVERY check on the head (required or not) must reach a passing terminal state before merge; a red or still-pending check blocks (the all-gates-blocking flip retired the curated *meant-to-block* allow-list the #923-class escapes grew one at a time). The single exemption is a check whose **name** contains `advisory` — one PR lane uses it: `Mobile texture-guard smoke (…, advisory)`, a genuinely-flaky llvmpipe render lane whose `--spawn` child hangs on ~half of runs (backlogged for a fix + re-graduation); three sanctioned step-level masks survive inside otherwise-blocking checks (fuzz-smoke's stochastic run, bucket-C's golden diff, bucket-E's Mesa per-test run — each documented at the step; the lanes' broken-harness teeth still block; cpp-lint blocks via its catch-all [error] tier with the cppcheck step report-only); **(2) CodeRabbit** — `APPROVED` / `COMMENTED + 0 actionable` passes, findings / changes-requested / review-skipped block (`cr-out-of-band` label downgrades a CR block to WARN); **(3) User comments** — zero unresolved non-bot non-self threads or conversation comments; **(4) Cursor Bugbot** — unresolved `cursor[bot]` inline findings block (`bugbot-out-of-band` downgrades to WARN), while a usage-cap / silent / stale Bugbot never wedges (grace + no-wedge hatches). Plus PR OPEN, `reviewDecision ∈ {APPROVED, null}`, no pagination overflow. A PR diffing **entirely** under `docs/self-improvement/**` auto-exempts CR (#2, via the `.coderabbit.yaml` path_filter) + Bugbot (#4, via `selfImpOnly`) with **no** label — CI + user gates still bind ([`merge-gates.md`](docs/agent-rules/merge-gates.md) § Self-improvement doc PR auto-exemption).

**Auto-merge applies when authorised** — a standing `governance.auto_merge: on` grant (`project.config.json`; SessionStart `## === auto-merge: on ===` banner) OR per-PR ("Register with watcher" / "merge when green"); `MERGE_GATES_FLIP_READY` / `SKIP_MERGE_GATES` / `*-out-of-band` labels + the REST-merge contract: [`merge-gates.md`](docs/agent-rules/merge-gates.md). **Never merge past ANY red check — required or not** (a non-required RED is real breakage; exceptions: an override label that *names* the check, or a positively-confirmed flake; admin-merge is only for a **stale-BLOCKED** state where everything is actually green), and **never trust a CodeRabbit "✅ Addressed" annotation blindly** — it matches commit keywords, not the diff; read the cited commit. (Incidents in [`postmortems.md`](docs/self-improvement/postmortems.md).)

Full per-outcome semantics + halt-prompt return-code table + env knobs + REST contract + merge-throughput / merge-queue `merge_group` detail: [`docs/agent-rules/merge-gates.md`](docs/agent-rules/merge-gates.md). Tests: `tests/bats/merge_gates.bats`.

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

**Prompt/contract size**: agent prompts (`agents/core/*.md` + `agents/project/*.md`) stay ≤ **250 lines** (soft-warn 150); `AGENTS.md` stays ≤ **150 lines** (navigation-only; soft-warn 120); the extraction *sinks* (`docs/agent-rules/*.md` + `agents/_shared/skills/**`) are **soft-warn-only** (≈400, never block — a hard cap there would fight the extraction they receive). Delta-gated by `agent_size_audit.py --diff origin/develop` (rule id `agent-too-long`); existing over-cap files are grandfathered ([`docs/high-integrity/agent-size-baseline.md`](docs/high-integrity/agent-size-baseline.md)) — only NEW files over cap or a file crossing its cap fail; an HTML-comment marker `<!-- SMATCHET_DEVIATION(rule=agent-too-long; …) -->` anywhere in the file escapes (prose/backtick mentions of the token do NOT — a bare-substring match once let this very sentence exempt `AGENTS.md` from its own cap). Shrink the whales by extracting agent procedure-bodies to skills + `AGENTS.md` rule-detail to `docs/agent-rules/*` per [`docs/agent-rules/AGENT-VS-SKILL.md`](docs/agent-rules/AGENT-VS-SKILL.md).

**Enforcement contract-card** — the gated rule-ids + zones + caps (single source of truth; each gate's `--selftest` asserts these tokens live here; full mechanics → [`cpp-rules.md`](docs/agent-rules/cpp-rules.md) § Tiered enforcement). Run all gates locally before every push: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` (or `bash scripts/dev/pre-ship.sh`). Delta-gated vs `origin/develop`, existing violators grandfathered, `SMATCHET_DEVIATION(rule=<id>; reason=…; owner=…; revisit=…)` escapes.

| Rule | Scope | Cap / note |
|---|---|---|
| `no-printf-stderr`, `define-imgui` | strict zone | grep rules |
| `no-raw-new`, `deviation-overdue`, `no-detach` | all first-party C++ | absolute (0; no grandfathering) |
| `no-glfw-in-core-headers` | `Source/Core/include/**/*.{h,hpp}` | absolute (0; GLFW/glad/OpenGL include breaks the DX12 dual-target build) |
| `cmake-local-gate-ci-scope` | `CMakeLists.txt` / `cmake/*.cmake` | absolute (0; a `message(FATAL_ERROR …)` keyed on a local knob `msvc_toolset_pin` without a `NOT DEFINED ENV{CI}` scope FATALs every fresh-configure CI runner — #1074) |
| `ui-request-flag-off-thread` | `Source/Core/src/Commands/**` (excl. `Scenarios/`) | absolute (0; a `g_ui.request{Window*,Screenshot*}` write in a command handler outside a `RunOnUiThread*` closure races the main loop polling those non-atomic fields — Pillar-3 data race; Scenarios run on the UI thread by contract) |
| `bare-json-parse-untrusted` | changed first-party `**/*.{cpp,h,hpp}` (repo-wide default-deny; the curated TU allow-list is retired — it lagged the code every recurrence: #1573/#1592/#1598) | **blocking**: a bare `nlohmann::json::parse(` — or a `stream >> json` slurp — not routed via `json_safe::ParseBounded` stack-overflows the recursive `~json` DOM teardown before any try/catch (#1271/#1287); the 3-arg non-throwing form still overflows. `SMATCHET_DEVIATION(rule=bare-json-parse-untrusted; …)` above the parse escapes program-internal bytes; tree is clean (`--scan-bare-json` empty, bats-asserted) |
| `catch-all-swallow` | all first-party C++ | absolute (0): an EMPTY `catch (...) {}` body silently swallows every exception — [`exception-handling-policy.md`](docs/agent-rules/exception-handling-policy.md) hard rule 1. A justifying comment inside the body, `// catch-all-ok:` on the catch line, or a deviation escapes |
| `unbounded-recursive-json-walker`, `unbounded-file-slurp` (WARN) | changed first-party C++ | advisory (calibration): a self-recursive `nlohmann::json`/`sol::object` walker with no depth/budget token (the DW class — #1220/#1237), and an uncapped `rdbuf()`/istreambuf whole-file slurp (SECURITY_AUDIT #33 class). Whole-tree sweeps: `--scan-json-walkers` / `--scan-slurps` |
| `unused-symbol-under-config-guard` (WARN) | changed first-party C++ `**/*.cpp` | advisory (calibration): a free-function def unguarded while ALL its refs are under a positive `#if defined(SMATCHET_WITH_*)` is dead in the feature-OFF build → Clang `-Werror,-Wunused-function` — the #863 config-skew escape (fixed by #945). WARN-first per the original `duplication` calibration precedent (per-file text proxy, not the compiler); nightly Lua-OFF sanitizer build is the authoritative backstop |
| `comment-commented-out-code`, `comment-decorative-banner`, `comment-blank-run` | all first-party C++ | comment-regrowth |
| `pr-numbered-temporal-comments` (WARN) | changed first-party C++ `**/*.{cpp,h,hpp}` comments | advisory (calibration): a comment pinning a DEV pull-request number (`// PR 5`, `// PR #1104`, `PR#1218`, `PR12`) is a temporal scaffold that rots once the PR squash-merges — rewrite to durable present-tense intent. Narrow regex (`PR` + optional space/`#` + digit) so product-domain "PR" (no number — `PR-only`, `type:pr`, `per-PR`) + Issue/ADR refs never match. WARN-first per the original `duplication` calibration precedent |
| `function-too-long` | all first-party C++ | **120** lines non-UI / **200** ImGui-draw (path under `Ui/` OR name starts `Draw`/`Render`) |
| `function-too-branchy` | all first-party C++ | **30** decision points |
| `include-cycle` | `Source/Core/**` quote-includes | acyclicity + layer-DAG; delta-gated vs origin/develop; baseline-grandfathered; SCC>1 or low→high back-edge fails |
| `no-ui-include-in-domain` | `Source/Core/{src,include}/{Tracker,Sync,Persistence,Config}/`, `Source/Plugins/Mcp/` | absolute (0; a quote-form `#include "Ui/…"` in domain code inverts the layer DAG — the include-cycle back-edge check is header→header by design, so a domain-TU→Ui edge escapes it; Commands/ excluded — sanctioned Scenario/view-visibility Ui seams) |
| `app-controller-fan-in` (`appcontroller_fan_in_audit.py`) | `Source/**` quote-form `#include "AppController.h"` | cap = baseline fan-in (currently 115; enforced as a merge-base delta, not the constant), **ratchet-down only**, **hard-FAIL absolute** (intentionally *not* WARN-first, unlike the `unused-symbol` calibration gate — a new includer is an exact signal); `SMATCHET_DEVIATION(rule=app-controller-fan-in; …)` above the include escapes a genuinely-needed new includer |
| `agent-too-long` (`agent_size_audit.py`) | agent prompts / `AGENTS.md` | **250** / **150** lines |
| `duplication` (`dup_audit.py`) | all first-party C++ | copy-paste clone, delta-gated vs origin/develop; **blocking** (graduated from WARN-first 2026-06-21, ADR-0015); `SMATCHET_DEVIATION(rule=duplication)` on/above either occurrence exempts |
| `interface-doc` (WARN) | `ITracker*.h`/`Tracker/*Client.h` ↔ `Tracker/AGENTS.md` | advisory: a doc-pinned `Type::method` changed in the header without a doc touch (symbol-pinned, not coarse — noise-spike-rejected) |
| `tu-line-ceiling` (WARN) | changed first-party `Source/**/*.cpp` (headers + ThirdParty out of scope) | advisory (god-file-splits regression guard): a touched translation unit over **1,200** lines — consider a cohesive companion-TU partition (the ceiling every god-file-splits TU landed under). Delta-scoped so the grandfathered pre-ceiling whales stay quiet until touched; `SMATCHET_DEVIATION(rule=tu-line-ceiling; …)` anywhere in the file escapes |
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
| authoring a multi-agent Workflow fan-out / background fleet (scoping · model pinning · concurrency · staging · checkpoints · salvage) | [`workflow-fleets.md`](docs/agent-rules/workflow-fleets.md) |

**Subsystem guides**: when you touch `Source/Core/src/<ctx>/`, read its leaf `AGENTS.md` first — single source of truth, overrides any central summary. Registry: [`CONTEXT-MAP.md`](CONTEXT-MAP.md) (detail in [`cpp-rules.md`](docs/agent-rules/cpp-rules.md) § Subsystem guides).

**Concurrent sessions**: one worktree per session (`nsc <slug>`) — a shared-tree `checkout`/`pull`/`reset` rug-pulls a sibling's HEAD; HEAD-drift guard + `resync` recovery + self-excluding `git-janitor` / `merge-watcher` auto-act confinement in [`process-rules.md`](docs/agent-rules/process-rules.md) § Concurrent interactive sessions.

## Process rules

How agents move work through the pipeline — full text + canonical recipes + carve-out list + the deferred-lint pipeline: [`docs/agent-rules/process-rules.md`](docs/agent-rules/process-rules.md).

- **Plan-doc family** — every plan at `docs/plans/active/<slug>.md`, committed immediately (`wip(plan): <slug>`; working-tree-only files are lost on checkout); post-ship update § Implementation log / Deviations / Verification; `grill-with-docs` before finalising; **Perf-gate section mandatory** when the diff touches `Source/Core/`. **Plan-revision pushes are PR-only** (never direct-push to develop). On scope-reduction, grep every deferred symbol across `**/CONTEXT*.md` / `docs/adr/` / `agents/*.md` / `docs/self-improvement/categories/` and clear stale "deferred-as-current" refs.
- **Git/p4 discipline** — 5-step pre-flight before any destructive git op (`reset --hard` / `clean -f` / `branch -D`) on a worktree this session didn't personally check out; force-push banned except a narrow `--force-with-lease` carve-out on `claude/<id>/*` + `agent/<task-stream-id>/*` branches during API-500 recovery. **Worktree-absolute path discipline**: under `.claude/worktrees/<id>/`, all `Edit`/`Write` paths use the worktree prefix (a main-repo prefix contaminates whatever branch main has checked out).
- **Cadence + verification** — `cmake --build` + `scripts/dev/test-all.sh` run **at most once per slice**, after implementation; pure-docs slices skip both (`is-pure-docs-diff.sh`); **stale-`Edit` recovery** = Re-Read → diff intended change → Re-Edit (never `replace_all` as force-write); deferred lint drains end-of-turn, `clang-format -i` inline. Deferred/skipped § Files-to-modify rows need a same-turn § Deviations + backlog entry.
- **Memory drain** — the auto-memory inbox (`~/.claude/projects/<slug>/memory/`) is transient; a SessionStart nudge fires at ≥ 5 items or > 7 days old → `/drain-memory` (triage each: **implement** → AGENTS.md/`docs/agent-rules/*`, **backlog** → `categories/*`, or **toss** — verify each claim first). Spec: [`docs/agent-rules/memory-drain.md`](docs/agent-rules/memory-drain.md).
- **Where new rules go** — 1-liners → § Project rules; topic-fit → that rule-doc; > 30-line topics → own `docs/agent-rules/<topic>.md` + stub; ≤ 30-line orphans → `process-rules.md`.

## Debug techniques

Pink-clear UI gap detection + the exe-staleness check → [`docs/agent-rules/debug-techniques.md`](docs/agent-rules/debug-techniques.md) (project-wide, mandatory whenever they apply). The full behavioural-bug investigation loop is `agents/core/debug-detective.md` + its `debug-instrument` skill.

## Semantic codebase search — use it first

Use **semantic codebase search first** for any "where is X / what calls Y / what does this touch" question (faster + more accurate than `grep` over a multi-MLOC tree); prefer **targeted reads / compact skeletons** for files you inspect but don't edit. Claude Code resolves this to a precedence ladder: **vexp `run_pipeline`** (primary — one call returns graph-ranked context + impact + memory; a PreToolUse hook blocks `Grep`/`Glob` while its daemon is up) → `Grep`/`Glob` for literal sweeps or when the daemon is down; other harnesses substitute equivalents (§ Harness adapter). These are concrete examples — the capability ("nav by meaning before raw text-search") is what matters. (The Sourcetrail `st_query.py` rung is retired — upstream discontinued; its prebuilt symbol DB never existed in a fresh checkout.)

**Semantic-search exceptions**: use **text-search** (not semantic) for **exhaustive literal/symbol inventories** (graph-ranked results aren't exhaustive — run once in the orchestrator, pass `<file>:<line>:<role>` matches inline to agents) and **mechanical renames / cleanup checks** (every occurrence must be found). Semantic search stays primary for **impact / ownership / surrounding-logic** understanding (the default path).

## Agent file locations

Canonical source: `agents/{core,project}/<name>.md` (per [agents.md spec](https://agents.md/)); shared scripts + skills at `agents/_shared/`. **Agent vs skill** rubric: [`docs/agent-rules/AGENT-VS-SKILL.md`](docs/agent-rules/AGENT-VS-SKILL.md) (skill = bounded-deterministic-inline; agent = exploration / loop / spawn / delegates). Per-harness adapter dirs (`.claude/`, `.cursor/`, `.pi/`, `.codex/`) are gitignored; Codex reads `AGENTS.md` natively and `setup-harness.sh` also generates a `.codex/` mirror (agents + config). Fresh clone: [`docs/harness/SETUP.md`](docs/harness/SETUP.md).

## Delegation

Default: stay in the orchestrator's primary model for routine work; delegate to an agent in `agents/` when the task matches. Full content — the **Orchestrator delegation packet**, **Parallel dispatch**, the context-budget ceiling, session-scratchpad + **Subagent progress markers** protocols, the **Agent output contract** (5-class table + `## Outcome:` mandate), **Trigger auto-activation**, the **Debug-mode pause-loop** (overrides ship-loop), **API-500 mid-run recovery**, **Agent versioning**, the cross-cutting + subsystem-specialist tables, and the delegate-vs-handle **Heuristic** — all in [`docs/agent-rules/delegation.md`](docs/agent-rules/delegation.md). External `AGENTS.md § <subsection>` refs resolve there; don't maintain parallel copies.

## Self-improvement loop

Every delegated agent ends with a `## Self-improvement` section — **empty is the common case and fine** (flag only real friction, never invent). Operational rules (format, categories, P0–P3, threshold, cadence) + index: [`docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`](docs/self-improvement/AGENT_SELF_IMPROVEMENT.md); **new entries are one file per entry** at `docs/self-improvement/categories/<cat>/<YYYY-MM-DD>-<slug>.md` (disjoint paths → no concurrent-add conflict; legacy monolith `categories/<cat>.md` still read in union), applied ones archive to `applied.md`.

**Gate escapes owe a postmortem** — anything that shipped to `develop` a gate should have caught (non-SUCCESS check at merge, override label, `Revert`, overdue deviation) is the highest-signal "gate, don't trust" lesson. `agents/scripts/core/postmortem-owed.sh` raises a SessionStart nudge; the [`gate-escape-postmortem`](agents/_shared/skills/gate-escape-postmortem/SKILL.md) skill runs a blameless RCA whose **mandatory** `### Preventing gate` names a new gate, appended to the [`docs/self-improvement/postmortems.md`](docs/self-improvement/postmortems.md) ledger.

## Dual-VCS topology (Perforce as opt-in local layer)

git/GitHub is the **ship-line** (PR review, CI, `smatchet-merge-watcher`); Perforce is an **opt-in local layer** (`SMATCHET_AGENT_VCS=p4`; default `git`) for agentic-WIP primitives (shelves, plan-lock counters, `+l` locks, task streams) — purely additive, never required / authoritative / on the ship-line. Mapping + lock discipline + shelf-vs-stash + destructive-p4 pre-flight: [`docs/perforce/AGENT_FLOWS.md`](docs/perforce/AGENT_FLOWS.md). Bring-up: [`docs/perforce/SETUP.md`](docs/perforce/SETUP.md). Janitor: [`agents/core/p4-janitor.md`](agents/core/p4-janitor.md).

## Harness adapter

Each agent declares a closed set of **capability tags**; the orchestrator (and the harness) maps tags to concrete tools. The full capability-tag → per-harness tool table + the per-harness discovery notes live in [`docs/harness/capability-adapter.md`](docs/harness/capability-adapter.md) (load when porting to a new harness or resolving what a tag means here). **Per-agent harness hints**: each agent's YAML frontmatter may carry a `harness-hints.<harness>:` block with harness-specific routing details (e.g. Anthropic model selection, MCP tool list); harnesses ignore unknown blocks.

**Recommended companion — caveman**: output-token compressor (~75% cut, technical content preserved byte-for-byte). Install + use instructions: [`docs/guides/caveman.md`](docs/guides/caveman.md). Default: `/caveman full` at session start.
