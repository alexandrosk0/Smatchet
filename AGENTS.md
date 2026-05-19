# Smatchet — agent + project rules

This is the canonical entry-point doc for any agentic harness (Claude Code, Codex / OpenAI Agents, Cursor, Aider, generic). Everything an agent needs lives in this repo — no external dependencies on user-global or parent-dir config.

## UX Pillars

Four north-star quality invariants for Smatchet. Pillars 1-3 are **enforceable** — agents auto-fail PRs that violate them. Pillar 4 is **aspirational** today — flagged in `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (category `process`), not a merge block, until the supporting infrastructure lands.

### 1. Performance — sustain ≈ 144 Hz

**Pillar 1**: sustained 144 Hz on the UI thread. Frame budget = **6.94 ms** (`1000 / 144`) under representative load.

**Enforceable invariants:**
- Steady-state mean per-frame UI work `≤ 6.94 ms` measured by `perf.snapshot` over a representative scenario.
- 60 Hz floor: no single frame > **16.67 ms** in normal operation; >16.67 ms outliers are spike-tracked at p99.
- `perf-detective` regression-fails any commit that lifts steady-state mean above budget on the same scenario.
- `spike-hunter` regression-fails any commit that introduces a new p99 > 16.67 ms on the UI thread under a previously-clean scenario.

**Tools**: `SMATCHET_UI_PERF_SCOPE("perf_temp:...")` markers per `agents/perf-instrument.md`; `perf.reset` → `scenario.run` → `perf.snapshot` loop per `agents/perf-measure.md`; `docs/PERF_WORKFLOW.md` for full ladder.

### 2. UI never freezes — predictable visual cue if it must

**Pillar 2**: zero manual verification steps; the UI thread never blocks longer than 100 ms without a visible cue. Any operation estimated **> 100 ms** moves to a worker thread. Synchronous I/O (HTTP, SQLite, p4, filesystem, blocking lock) reaching the UI thread = **code-review CRITICAL**.

**Visual cue contract** for the rare unavoidable blocking case:
- Spinner or progress widget appears within **100 ms** of op start.
- Cancelable when the underlying op supports it (HTTP, p4, long-running queries).
- Modeless when possible; modal only when the result is required to proceed.
- No silent waits — the user is never left guessing whether the app is alive.

**Enforceable invariants:**
- `code-review` flags any new synchronous call to `cpr`, `SQLite::Database`, `p4 …`, `std::ifstream`-on-disk, or `std::mutex::lock` from a function reachable from `ImGui::*`-frame as Critical.
- `spike-hunter` enforces UI-thread p99 < 100 ms on the standard scenario; cue-less hitches above that line block merge.

**Worker-thread hand-off**: post results back to the UI thread via `MainThreadDispatcher` (`Source_Core/include/MainThreadDispatcher.h`); never touch ImGui state directly from a worker.

### 3. Never crash

**Pillar 3**: Smatchet must terminate cleanly under all observed inputs. Crashes in dev block the next merge until fixed; crashes in shipped builds are P0 regressions.

**Enforceable invariants:**
- **Pre-merge sanitizer build** mandatory on any PR that touches `Source_Core/` C++: `cmake --build --preset ninja-test-msys2` runs the doctest rig under ASan / UBSan (when toolchain supports it). `debug-detective` runs the sanitizer build for every crash-suspect investigation.
- **RAII enforced**: no raw `new` / `delete` outside the documented edge cases (sol2 user data, ImGui callback shims). Use `std::unique_ptr` + `make_unique`. `code-review` flags raw heap ops.
- **Bounds-checked**: every container index goes through `at()` / explicit length check; `cppcheck` `boundsError` / `arrayIndexOutOfBounds` blocks merge.
- **No silent UB**: dereferenced `nullptr`, unsigned wrap-around, signed overflow, use-after-free — all blocking. UBSan output during the regression gate is a fail.
- **Graceful degradation in ship builds**: assertions fire in dev (`assert(...)`); in ship builds the same condition logs `LOG_ERROR` and the calling function returns a safe default. The app never aborts on a recoverable bad state.

### 4. Accessibility — aspirational (locked scope)

**Pillar 4**: keyboard nav, font-size / zoom, WCAG AA contrast. No auto-fail gates today. Agents flag missing a11y to `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (category `process`) so it accumulates evidence; pillar hardens once the supporting infra lands.

**Locked in-scope (work on these when adjacent to current task):**
- **Keyboard navigation**: every actionable widget reachable without mouse. Tab order sane, focus indicators visible, `Ctrl+Shift+P` Command Palette as the keyboard entry point to every registered command.
- **Font size / zoom**: user-controlled `ImGuiIO::FontGlobalScale`, persisted in `smatchet_config.json`. Affects grid row heights, cell renderers, and modal sizing.
- **Color contrast**: WCAG AA minimum — 4.5:1 for body text, 3:1 for large text and UI components — on both default and dark themes. Theme audit before any palette change.

**Out of scope (deferred until a concrete user need):**
- Screen-reader compatibility. ImGui has no native a11y tree; wiring one is a multi-week effort. Defer.
- High-contrast / inverted-color themes beyond the WCAG AA floor.

**Why aspirational, not enforceable**: there is no automated check for "is this widget keyboard-reachable" or "does this palette meet WCAG AA contrast" today. Adding such checks is its own work-stream; pillars 1-3 already block merges where they matter most.

### Agent ownership

| Pillar | Primary agent | Notes |
|---|---|---|
| 1. Performance | `perf-detective` (sustained), `spike-hunter` (intermittent), helpers: `perf-instrument`, `perf-measure` | See `docs/PERF_WORKFLOW.md`. |
| 2. UI never freezes | `code-review` (sync-on-UI sniff), `spike-hunter` (p99 enforcement), `debug-detective` (root-cause when a freeze ships) | UI-thread budget: any call reachable from `ImGui::*`-frame stack. |
| 3. Never crash | `debug-detective` (diagnose), `code-review` (RAII / bounds / nullptr review), `build-doctor` (sanitizer build gate) | Crashes block merge unconditionally. |
| 4. Accessibility | none today | Flag in backlog; reassess pillar hardening when keyboard-nav / zoom / contrast checks have automated test support. |

## Autonomous ship-loop default

**Rule**: orchestrator runs each user task end-to-end in **one turn** without pausing for confirmation at each stage. The default sequence:

```
diagnose → fix → build → commit → push → open PR → squash-merge → git-janitor cleanup → backlog entry
```

All clarifications that the orchestrator anticipates needing are batched **once at the start** via `AskUserQuestion`. Once the user answers, the loop proceeds without further prompts until completion (or until an exception below fires).

**Why a default**: harnesses that drip-step every stage create N round-trips for a task that needs one. The user already chose the task; the loop is the cheapest way to deliver it. Other harnesses (Codex / Cursor / Aider) read AGENTS.md and need the rule too — user-private memory is not portable.

**Exceptions** (loop pauses or stops):

1. **Debug-mode pause-loop** — user prompt matches the `debug-detective` trigger row (see § Delegation § Trigger auto-activation). The pause-loop in § Debug-mode pause-loop **overrides** the ship-loop for the duration of the investigation.
2. **Destructive ops outside loop** — `git reset --hard`, `git push --force` to a shared branch, `git branch -D`, `gh pr merge` of a non-self PR, `rm -rf` outside the worktree, schema drops. These require explicit confirmation per § Project rules § Destructive git ops in shared worktrees.
3. **Cross-repo or external-service mutations** — anything that writes outside the current repo or calls a third-party API with side effects (posting to Slack, sending email, modifying a Jira ticket the user didn't ask for). Confirm before acting.
4. **Anything not previously authorised in a durable rule** — durable = recorded in AGENTS.md, CLAUDE.md, or this session's explicit user instructions. Verbal "ok in this conversation" doesn't bind future turns; encode it as a memory or doc edit if it should.

### Post-ship turn-end protocol

After the loop reaches PR-opened (or the equivalent terminal state for the task), end the turn with `AskUserQuestion` offering the four canonical next steps as discrete options:

1. **Manual verify** — user wants to drive the change manually before merge.
2. **Review PR** — user wants to read the diff / comment on GitHub.
3. **Squash-merge** — user is satisfied; orchestrator merges + cleans up.
4. **Done** — no further action; PR stays draft for later.

Do **not** emit a free-form bulleted next-steps list — `AskUserQuestion` is a single click; prose is N seconds of composition.

**Skip-condition**: if the user has already said "no more changes coming" / "ship it and stop" / "merge when green" in the same turn, skip the question and hand off to `git-janitor` directly.

Cross-link: ship-loop reference in § Delegation; pause-loop override in § Delegation § Debug-mode pause-loop.

## Project rules

**Build**: `cmake --build --preset ninja-iter-msys2` (iter), `ninja-debug-msys2` (debug), `ninja-publish-msys2` (publish). Exe at `build/<preset>/Smatchet.exe` (the CMake target is `SmatchetStandalone` but `OUTPUT_NAME` ships as `Smatchet`).

**Language**: C++14 hard (Unreal compat). Banned: `string_view`, `optional`, `variant`, structured bindings, `if constexpr`. Must compile on MinGW UCRT + MSVC.

**Layout**: `Source_Core/{src,include}` is the shared core — used by both standalone and Unreal. `Target_Standalone/` builds the OpenGL exe. `Plugins/{Mcp,LuaConsole}` are static plugins. `*_DX12` targets are `EXCLUDE_FROM_ALL` (Unreal only) — don't touch unless asked.

**Available libs** (FetchContent, linked): nlohmann/json, cpr, SQLiteCpp, cpp-httplib, md4c, ImGui (docking), GLFW, Lua + sol2, ghc::filesystem.

**Logging**: `LOG_{DEBUG,INFO,WARN,ERROR,TRACE}` from `Logger.h` — never `printf` / `std::cerr`.

**nlohmann json**: `obj["k"] = v`, not `obj = {...}` (reassignment with brace-list won't compile).

**Optional plugins**: gate with `#if SMATCHET_WITH_LUA_AUTOMATION` / `#if SMATCHET_WITH_MCP`. Lua bindings split: `AppController_LuaBindings.cpp` (on) ↔ `AppController_LuaStubs.cpp` (off) — keep in sync.

**Don't**: add GLFW/OpenGL includes to `Source_Core/` headers (DX12 compiles them too); redefine `IMGUI_USE_WCHAR32` (PUBLIC on `ImGuiLib`).

**Dual-target**: `Source_Core/` compiles into both `SmatchetStandalone` (OpenGL+GLFW) and `SmatchetCore_DX12` (Unreal). Diverging macros: `SMATCHET_EMBEDDED_IN_UNREAL=1` (DX12 only); `SMATCHET_WITH_MCP=1` (Standalone only — `SMATCHET_WITH_MCP_UNREAL` is OFF). Full verify: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`.

**Quality**: RAII (no raw `new`/`delete` — use `std::unique_ptr` + `make_unique`); `const&` for non-trivial params; `std::move` on last use; small focused functions; `LOG_TRACE`/`LOG_DEBUG` in non-trivial branches.

**Lint**: your harness may run an automatic lint pass after C++ edits. Claude Code does so via a `PostToolUse` hook wired by `bash scripts/setup-harness.sh claude-code` — `clang-format -i` applies in place; `cppcheck` + `clang-tidy` report to stderr. If your harness lacks hook automation, run those three tools manually on every edited `.cpp` / `.h` in `Source_Core` / `Plugins` / `Target_Standalone` and fix all reported issues before responding.

**Perf workflow**: when the user asks to optimize / profile / fix FPS / lag / hitch / "slow" / spike, read [`docs/PERF_WORKFLOW.md`](docs/PERF_WORKFLOW.md) and follow it. Don't load it for unrelated tasks.

**Plan location**: every plan / design doc lives under `docs/design/<slug>.md`. No plans in repo root, `backlog/`, `~/.claude/plans/`, or working-tree-only scratch. `backlog/` is for triage lists (CPPCHECK_PLAN, AGENT_SELF_IMPROVEMENT) — not new plans. Naming: kebab-case slug matching the feature (`vs-style-view-menu.md`, `remove-global-project-key.md`).

**Plan-doc safety**: as soon as a plan is written to `docs/design/<slug>.md`, `git add` + commit it immediately with a `wip(plan): <slug>` prefix before any other work or branch operation. Working-tree-only files are silently lost on `git checkout`, `git reset --hard`, or GitHub Desktop branch switches. Recovery via `git fsck --lost-found` is expensive. Never leave a plan untracked across a session boundary.

**Destructive git ops in shared worktrees**: before running any destructive git op (`reset --hard`, `checkout --`, `clean -f`, `branch -D`) against a worktree the orchestrator did not personally check out earlier in the same session, run a mandatory 5-step pre-flight via `git -C <path>` from the orchestrator's main worktree (do not `cd`). Parallel agents in other worktrees can — and do — switch the target worktree's HEAD to a different branch between sessions; a stale assumption about "the develop worktree" is what destroys uncommitted work.

1. `git -C <path> branch --show-current` — verify the actual current branch matches the user-named target. If it doesn't, **stop**; the worktree has been reassigned.
2. `git -C <path> status --short` — inventory tracked-modified + untracked files. Any non-empty result means the worktree is load-bearing for a parallel agent; treat as a refusal condition unless explicitly authorised.
3. `git -C <path> stash push -m "<reason>" -- <modified-files>` for any tracked-modified files, regardless of apparent relevance to the asked task. Untracked files survive `reset --hard` but `clean -f` is fatal — pass `--include-untracked` when `clean -f` is part of the plan.
4. Run the destructive op only after 1-3 succeed.
5. Decide whether to `stash pop` (recovers the user's work), leave the stash for the human (when unrelated to current PR), or report the stash hash so the human can pop manually if conflicts arise.

`reset --hard` permanently destroys uncommitted tracked-modified content; it is not in reflog. Branch pointers are reflog-recoverable; uncommitted changes are not. Cross-link: `agents/git-janitor.md` § Destructive-op pre-flight (authoritative checklist).

**Plan revision after implementation**: when work shipped from a plan lands (PR merged, scenario validated, or feature shipped), edit the originating `docs/design/<slug>.md` in the same or next commit to record what actually happened. Mandatory sections to append:

- `## Implementation log` — bullet per shipped commit: `<sha> · <one-line summary>`.
- `## Deviations from plan` — what was changed, removed, or deferred relative to the original plan, with one-line rationale per item.
- `## Verification` — what was actually tested + result (passed / failed / not-run).

A plan that ships without revision is a stale plan. Future agents read these docs as truth; drift between plan and shipped reality is the main cost of multi-week feature work.

**Plan stress-test — `grill-with-docs` skill**: before finalising `docs/design/<slug>.md`, invoke the skill to grill the plan against `docs/CONTEXT.md` (glossary) and `docs/adr/` (ADRs). Outputs: refined plan + glossary updates + new ADRs only when hard-to-reverse + surprising + real-trade-off all fire. Smatchet file mapping in `agents/_shared/skills/grill-with-docs/SMATCHET-NOTES.md`.

**Verification automation — zero manual steps**: `test-author` converts every manual verification step into a deterministic CLI / scenario / screenshot / sanitizer / ImGui-Test-Engine assertion. Three invocation points: (1) plan-time audit of `docs/design/<slug>.md` § Verification, (2) post-first-round sweep, (3) every agent handoff that mentions a manual step. Unified runner: `bash scripts/dev/test-all.sh` (auto-enrols `scripts/dev/test-*.sh`). Manual residue without a `docs/backlog/AGENT_SELF_IMPROVEMENT.md` entry (category `tooling`) is a fail. "Truly interactive" is never the final answer — bucket E (ImGui Test Engine) hasn't been wired yet. Bucket details in `agents/test-author.md`.

**Schema-version bumps**: when a feature requires a config / cache schema-version bump, hold the bump until the feature is verified end-to-end. Do not commit interim version bumps as the feature evolves — squash or amend. The shipped version should be exactly one higher than the previous shipped version, not N higher because of intermediate iterations.

**Trivial-visual-only change envelope**: a change qualifies as **trivial-visual** when **every** condition holds — (a) write set is a strict subset of `{Source_Core/src/SmatchetTheme.cpp, Locales/*.json, ImGui style constants (`ImVec4` / `ImGuiStyle` literals)}`; (b) diff shape is literals-only (no API surface, no header touch, no schema, no control flow, no new symbols); (c) zero touch under `Source_Core/include/` / `Plugins/` / `cmake/` / `CMakePresets.json`. Under this envelope the orchestrator may ship after: (1) `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone` builds, (2) `ninja-test-msys2` ctest passes **if** a pure-logic test touches the changed file, (3) **NO** `ninja-ui-test-msys2` bucket-E run, (4) **NO** isolated worktree — use the main worktree with `git stash` race-recovery if a concurrent agent appears. Bucket-E coverage gets deferred to a single post-batch run before merge, or to the test backlog. Saves ~10× wall-clock on visual-only PRs (palette retunes, locale string fixes). Any condition fails → fall back to the full build + test-all + bucket-E loop.

**Build / ctest cadence — slice-boundary only**: within a single agent turn (= one logical slice), invoke `cmake --build` and `scripts/dev/test-all.sh` **at most once each**, and only after the implementation is complete. Mid-slice rebuilds and mid-slice ctest runs are wasted work — Ninja is already incremental and the doctest rig is fast at the slice boundary but expensive when amortised across N edits.

The harness maintains a `.claude/.tree-dirty` sentinel file written by `.claude/hooks/lint-cpp.sh` on every first-party `.cpp` / `.h` edit and cleared automatically by the `PreToolUse:Bash` hook (`clear-tree-dirty.sh`) the moment any `cmake --build …` invocation is about to run. Agents reading the sentinel know edits have happened since the last build — if your implementation isn't done yet, defer the build.

The deferred lint pipeline (`.claude/hooks/lint-cpp.sh` PostToolUse → `.claude/hooks/lint-cpp-drain.sh` Stop) follows the same principle for `cppcheck` / `clang-tidy` / dual-target syntax: heavy passes drain once at end-of-turn against the dedup'd set of edited files, not after each Edit/Write. `clang-format -i` still runs inline. Escape hatches: `SMATCHET_LINT_INLINE=1` reverts to per-edit, `bash scripts/dev/lint-flush.sh` drains explicitly mid-turn. The trivial-visual-only envelope above is a special case of this rule.

## Debug techniques

**Pink-clear UI gap detection**: for "is the background ever visible behind panels?" / "are dock gaps still leaking?" questions, set the clear color to magenta (`glClearColor(1.0f, 0.0f, 1.0f, 1.0f)` on Standalone, equivalent `ClearRenderTargetView` color on DX12). Any visible pink is a guaranteed dock gap or transparent region. Pair with a screenshot + per-pixel pink scan for objective regression tests.

**Exe staleness check**: after every rebuild, `ls -la` both the patched output and the most-likely-stale exe paths side-by-side, compare mtimes, and name the **exact** path the user should run. Multiple build outputs (`build/ninja-iter-msys2/`, `build/ninja-release/`, worktree builds) make wrong-exe testing a common time-sink — orchestrator + perf / build agents all enforce this.

## Semantic codebase search — use it first

Every agent in this repo expects the orchestrator (or the agent itself) to use **semantic codebase search** before falling back to text-search. In practice that means:

- **Always** call the harness's indexed codebase search first for any "where is X" / "what calls Y" / "what does this touch" question. This is faster, cheaper, and more accurate than raw `grep` over a multi-MLOC codebase.
- Prefer **compact file-skeleton views** (signatures + classes only) for files you're inspecting but not editing — typically 70–90% token savings vs full reads.
- Fall back to text-search + full reads only when no semantic search is available or its index is degraded.

Under Claude Code this maps to `mcp__vexp__run_pipeline` (semantic search) and `mcp__vexp__get_skeleton` (skeleton). Other harnesses substitute their equivalents (see the Harness adapter table below). Agents whose prose mentions vexp do so as a concrete example — the capability is what matters.

## Agent file locations

Canonical, single source of truth: `agents/<name>.md` at the repo root (per the [agents.md spec](https://agents.md/)). Shared scripts + skills live at `agents/_shared/`.

Per-harness adapter directories (`.claude/`, `.codex/`, `.cursor/`) are **gitignored** — they're regenerated locally from the canonical tree by `bash scripts/setup-harness.sh <name>`. Adapters use directory junctions / symlinks where possible so edits to `agents/*.md` are picked up by the harness immediately, no sync step.

First-time setup or fresh clone? See [`docs/harness/SETUP.md`](docs/harness/SETUP.md).

## Delegation

**Moved to** [`docs/agent-rules/DELEGATION.md`](docs/agent-rules/DELEGATION.md) (~230 lines lifted out for navigability — AGENTS.md is now ~320 lines instead of 549).

Default: stay in the orchestrator's primary model for routine work. Delegate to an agent in `agents/` when the task matches.

Quick index of moved subsections — full content in `docs/agent-rules/DELEGATION.md`:

- **Orchestrator delegation packet** — plan-lock pre-flight, shared inventory, invariant decisions, output budget, plan revision contract, subagent progress markers reminder, pure-helper TU-split recipe.
- **Parallel dispatch** — when to run multiple subagents in one tool-use block.
- **Session scratchpad protocol** — `.session-context.md` lifecycle + `## Session context append` shape.
- **Subagent progress markers** — `.progress.log` via `bash scripts/dev/agent-progress.sh`.
- **Tool-trace contract** — hook-derived; agents don't track manually.
- **Agent output contract** — 5-class table (Investigator / Diagnostic read-edit / Implementer / Helper / Maintenance) + `## Outcome:` mandate.
- **Trigger auto-activation** — keyword → agent routing table.
- **Debug-mode pause-loop (overrides ship-loop)** — for `debug-detective` triggers.
- **Skeleton-first** — `get_skeleton` for inspection, `Read` for editing.
- **Agent versioning** — when to bump `version: <N>`.
- **Cross-cutting** + **Subsystem specialists** — delegation tables.
- **Stay in the orchestrator for** — routine work list.
- **Heuristic** — when to delegate vs handle directly.
- **`delegates-to:` frontmatter** — direct call vs orchestrator-routed.
- **Why split** + **Complexity rationale** — design intent.

External references to `AGENTS.md § <subsection>` continue to resolve via this index — agents who read AGENTS.md land here, see the cross-link, and follow it to the canonical text. Don't maintain parallel copies; edit the canonical at `docs/agent-rules/DELEGATION.md`.


## Handoff envelope

The agentic coding-handoff flow (see `docs/design/agentic-coding-handoff.md`) spawns a `claude` child process inside an isolated git worktree to implement an approved `AgentProposal` of action `ImplementIssue`. The runner side (`ClaudeCodeLocalRunner` from H3 onward) and the spawned harness's first delegate (`handoff-implementer`) communicate through a fixed set of **sentinel files** living at the worktree root. Vocabulary, write contracts, and env boundaries are pinned here so all agents in the canonical tree share one language.

### Sentinel files

Six files, one writer + one reader per file. None of them are committed to the git history — the runner writes them before / during the child's lifetime; the child writes its results back; the runner consumes the results on exit.

| File | Writer | Reader | One-line role |
|---|---|---|---|
| `SEED.md` | runner | handoff-implementer | Human-readable handoff brief; weave into commit-message + PR-body prose. |
| `SEED.json` | runner | handoff-implementer | Canonical payload (`CodingHarness::Seed` struct); first thing the child reads on entry. |
| `CLARIFICATION_NEEDED.json` | handoff-implementer | runner + Smatchet UI | Written **only** when the child cannot proceed without user input; surfaces a single question, then the child stops. |
| `USER_RESPONSE.json` | runner | handoff-implementer | Written by the runner (or the Smatchet UI, or the GitHub-comment poller) when the user answers; child reads on resume. |
| `RUN_RESULT.json` | handoff-implementer | runner | Terminal signal — `{ ok, errorMessage, prUrl, filesChanged, linesAdded, linesRemoved, toolUseSummary }`. Runner watches for this file; its appearance flips the FSM to Complete or Failed. |
| `PR_URL.txt` | handoff-implementer | runner + Smatchet UI | Single line containing the PR URL. Mirrors `RUN_RESULT.json.prUrl` so the UI has a cheap path before parsing the full result. |

Write-once semantics: every sentinel except `USER_RESPONSE.json` (rewritten per clarification round) is written once per spawn. `RUN_RESULT.json` is the **last** write the child performs before exit — anything else can race, but the runner must observe `RUN_RESULT.json` strictly after `PR_URL.txt`.

### Env allow-list

The child process is spawned with a fresh environment block, NOT an inherited copy of the parent's. Allow-listed variables only:

```text
PATH, HOME, USER, USERPROFILE, TEMP, TMP, SYSTEMROOT, GH_TOKEN, GITHUB_TOKEN, ANTHROPIC_API_KEY
```

No other variables inherit. In particular, **`SMATCHET_*` never inherits** — Smatchet config and any secrets/PATs stored in `SMATCHET_*` env vars stay in the parent process and are unavailable to the child. The runner asserts that at least one of `GH_TOKEN` / `GITHUB_TOKEN` is set before spawning so the child's `gh pr create --draft` invocation can authenticate.

The env-list discipline is enforced by `ClaudeCodeLocalRunner` (lands in H3); the env-allow-list doctest in H3's test surface spawns the stub child with a poisoned `SMATCHET_SECRET=leak` and asserts the child's recorded env file shows the variable is absent.

### Branch naming

Worktree branches follow `agent/<proposalId>/<short-slug>` where `<short-slug>` is the first 32 characters of `kebab-case(issueTitle)` (truncated at a word boundary when possible). Two hard rules:

- The branch must not equal `develop` or `main`; the runner asserts this before `git worktree add` and the agent re-asserts it before any push.
- The branch must already exist in the worktree before the child is spawned; the child never runs `git checkout -b` itself.

### Worktree layout

Worktree root: `.claude/worktrees/agent-<proposalId>` (already gitignored via the existing `.claude/worktrees/` rule). One worktree per proposal; cleanup is a separate concern (`handoff.gc --older-than-days <n>` command, H4 onward).

### PR draft requirement

Every PR opened by the harness is `--draft`. The user marks ready-for-review only after auditing the diff. The harness never calls `gh pr merge`, never closes / reopens PRs, and never pushes to a non-`agent/*` branch.

### Spawned-orchestrator first-move contract

The spawned `claude` child's first move, if `SEED.json` exists at `$PWD`, is to delegate to `handoff-implementer` with the file as inline context. Do not re-read it; do not improvise routing. The delegate owns the diagnose → code → test → commit → push → PR loop and writes the terminal `RUN_RESULT.json` before exit.

### Anti-deception note

**Anti-deception note**: `HarnessRunState::IsTransitionAllowed` (`Source_Core/include/HarnessRunState.h`) is the FSM integrity boundary for the agentic handoff lifecycle. `AgenticHandoffController::ControllerTransition` validates every state-name string the runner emits through this predicate before audit-trailing + storing — disallowed transitions log `LOG_WARN` and are dropped. The check exists because the runner emits **untrusted strings** read from the spawned child's stdout; a compromised / buggy harness must not be able to forge `Spawning → Complete` or other skip-states. Loosening the FSM for any reason defeats the load-bearing piece. Keep the predicate strict; if a new legitimate transition emerges, add it explicitly to the allow-list rather than relaxing the check.

## Self-improvement loop

Every delegated agent ends its report with a `## Self-improvement` section. **Empty is the common case and explicitly fine** — agents only flag real friction, never make up suggestions.

Operational rules — format, categories (`bug` / `process` / `tooling` / `infra` / `test` / `security`), priority enum (P0–P3), workflow steps, apply threshold, triage cadence — live alongside the index at [`docs/backlog/AGENT_SELF_IMPROVEMENT.md`](docs/backlog/AGENT_SELF_IMPROVEMENT.md). Live entries split per category under [`docs/backlog/agent-self-improvement/`](docs/backlog/agent-self-improvement/). Applied entries archive immediately to `agent-self-improvement/applied.md`. The goal is a self-tightening loop — agents notice friction, the orchestrator accumulates evidence, prompts get patched, friction drops.

## Harness adapter

Each agent declares a closed set of **capability tags**. The orchestrator (and the harness) maps tags to concrete tools. Currently known mappings:

| Capability tag | Claude Code | Codex / OpenAI Agents | Cursor | Aider | Generic CLI |
|---|---|---|---|---|---|
| `semantic-code-search` | `mcp__vexp__run_pipeline` | vexp.run_pipeline (MCP) | (built-in search panel) | (not built-in — fall back to text-search) | `rg` over symbol set |
| `file-skeleton` | `mcp__vexp__get_skeleton` | vexp.get_skeleton (MCP) | — | — | `ctags -x <file>` |
| `file-read` | `Read` | `read_file` | (built-in) | (built-in) | `cat` |
| `file-edit` | `Edit` | `apply_patch` | (built-in) | (built-in) | `sed` / patch |
| `text-search` | `Grep` | `rg` (shell) | (built-in) | (built-in) | `grep` / `rg` |
| `file-glob` | `Glob` | shell `find` | — | — | `find` |
| `shell` | `Bash` | `shell` | terminal | shell | sh |
| `web-fetch` | `WebFetch` | `web.fetch` | — | — | `curl` |
| `git-history` | `Bash(git log)` | `shell(git log)` | (built-in) | (built-in) | `git log` |

**Harness notes:**

- **Claude Code** discovers agents at `.claude/agents/` — a junction into the canonical `agents/` tree created by `bash scripts/setup-harness.sh claude-code`. Edits to `agents/*.md` are visible immediately; no sync step.
- **Codex / OpenAI Agents** reads `AGENTS.md` per the [agents.md spec](https://agents.md/). Discover individual agents at `agents/*.md`.
- **Cursor / Aider / generic** — human-driven. Agent files at `agents/` are reference docs the user pastes or invokes manually.
- Harnesses without `semantic-code-search` should fall back to text-search with the symbol set named in each agent's prose. Output is degraded but workable — expect more round-trips and larger context per query.

**Per-agent harness hints**: each agent's YAML frontmatter may carry a `harness-hints.<harness>:` block with harness-specific routing details (e.g. Anthropic model selection, MCP tool list). Harnesses ignore unknown blocks.

## Recommended companion — caveman

Output-token compressor (~75% cut, technical content preserved byte-for-byte). Install + use instructions: [`docs/CAVEMAN.md`](docs/CAVEMAN.md). Default: `/caveman full` at session start.

## Semantic-search exceptions

The `## vexp` block below auto-regenerates on tool update — these carve-outs live outside it so they survive.

- **Exhaustive literal / symbol inventories**: use text-search (`rg` / harness equivalent), not semantic search. Graph-ranked results are not exhaustive. Run the search once in the orchestrator and pass `<file>:<line>:<role>` matches inline to delegated agents.
- **Mechanical renames and cleanup checks**: same — every occurrence must be found. `mechanic` and `perf-instrument` already use text-search per their prompts.
- **Understanding impact / ownership / surrounding logic**: semantic search stays primary. This is the default path.

## vexp <!-- vexp v1.2.28 -->

**MANDATORY: use `run_pipeline` — do NOT grep or glob the codebase.**
vexp returns pre-indexed, graph-ranked context in a single call.

### Workflow
1. `run_pipeline` with your task description — ALWAYS FIRST (replaces all other tools)
2. Make targeted changes based on the context returned
3. `run_pipeline` again only if you need more context

### Available MCP tools
- `run_pipeline` — **PRIMARY TOOL**. Runs capsule + impact + memory in 1 call.
  Auto-detects intent. Includes file content. Example: `run_pipeline({ "task": "fix auth bug" })`
- `get_context_capsule` — lightweight, for simple questions only
- `get_impact_graph` — impact analysis of a specific symbol
- `search_logic_flow` — execution paths between functions
- `get_skeleton` — compact file structure
- `index_status` — indexing status
- `get_session_context` — recall observations from sessions
- `search_memory` — cross-session search
- `save_observation` — persist insights (prefer run_pipeline's observation param)

### Agentic search
- Do NOT use built-in file search, grep, or codebase indexing — always call `run_pipeline` first
- If you spawn sub-agents or background tasks, pass them the context from `run_pipeline`
  rather than letting them search the codebase independently

### Smart Features
Intent auto-detection, hybrid ranking, session memory, auto-expanding budget.

### Multi-Repo
`run_pipeline` auto-queries all indexed repos. Use `repos: ["alias"]` to scope. Run `index_status` to see aliases.
<!-- /vexp -->
