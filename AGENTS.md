# Smatchet — agent + project rules

This is the canonical entry-point doc for any agentic harness (Claude Code, Codex / OpenAI Agents, Cursor, Aider, generic). Everything an agent needs lives in this repo — no external dependencies on user-global or parent-dir config.

## UX Pillars

Four north-star quality invariants for Smatchet. Pillars 1-3 are **enforceable** — agents auto-fail PRs that violate them. Pillar 4 is **aspirational** today — flagged in `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (category `process`), not a merge block, until the supporting infrastructure lands.

### 1. Performance — sustain ≈ 144 Hz

**Target**: 144 frames per second on the UI thread under representative load. Frame budget = **6.94 ms** (`1000 / 144`).

**Enforceable invariants:**
- Steady-state mean per-frame UI work `≤ 6.94 ms` measured by `perf.snapshot` over a representative scenario.
- 60 Hz floor: no single frame > **16.67 ms** in normal operation; >16.67 ms outliers are spike-tracked at p99.
- `perf-detective` regression-fails any commit that lifts steady-state mean above budget on the same scenario.
- `spike-hunter` regression-fails any commit that introduces a new p99 > 16.67 ms on the UI thread under a previously-clean scenario.

**Tools**: `SMATCHET_UI_PERF_SCOPE("perf_temp:...")` markers per `agents/perf-instrument.md`; `perf.reset` → `scenario.run` → `perf.snapshot` loop per `agents/perf-measure.md`; `docs/PERF_WORKFLOW.md` for full ladder.

### 2. UI never freezes — predictable visual cue if it must

**Rule**: Any operation estimated **> 100 ms** moves to a worker thread. Synchronous I/O (HTTP, SQLite, p4, filesystem, blocking lock) reaching the UI thread = **code-review CRITICAL**.

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

**Rule**: Smatchet must terminate cleanly under all observed inputs. Crashes in dev block the next merge until fixed; crashes in shipped builds are P0 regressions.

**Enforceable invariants:**
- **Pre-merge sanitizer build** mandatory on any PR that touches `Source_Core/` C++: `cmake --build --preset ninja-test-msys2` runs the doctest rig under ASan / UBSan (when toolchain supports it). `debug-detective` runs the sanitizer build for every crash-suspect investigation.
- **RAII enforced**: no raw `new` / `delete` outside the documented edge cases (sol2 user data, ImGui callback shims). Use `std::unique_ptr` + `make_unique`. `code-review` flags raw heap ops.
- **Bounds-checked**: every container index goes through `at()` / explicit length check; `cppcheck` `boundsError` / `arrayIndexOutOfBounds` blocks merge.
- **No silent UB**: dereferenced `nullptr`, unsigned wrap-around, signed overflow, use-after-free — all blocking. UBSan output during the regression gate is a fail.
- **Graceful degradation in ship builds**: assertions fire in dev (`assert(...)`); in ship builds the same condition logs `LOG_ERROR` and the calling function returns a safe default. The app never aborts on a recoverable bad state.

### 4. Accessibility — aspirational (locked scope)

**Status**: no auto-fail gates today. Agents flag missing a11y to `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (category `process`) so it accumulates evidence; pillar hardens once the supporting infra lands.

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

## Project rules

**Build**: `cmake --build --preset ninja-iter-msys2` (iter), `ninja-debug-msys2` (debug), `ninja-publish-msys2` (publish). Exe at `build/<preset>/SmatchetStandalone.exe`.

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

Default: stay in the orchestrator's primary model for routine work. Delegate to an agent in `agents/` when the task matches.

### Orchestrator delegation packet

Before delegating a design-doc PR or any multi-step implementation slice, build a compact handoff packet. This saves agents from re-reading the same docs, rediscovering the same call sites, or re-litigating invariants.

Each packet should include:

- **Parallel-plan pre-flight**: query the live ref-based plan-lock state via `bash scripts/dev/locks-show.sh` before composing the packet. The auto-generated Markdown snapshot at [`docs/design/_plan-locks.generated.md`](docs/design/_plan-locks.generated.md) may lag by up to 30 minutes (regenerated by [`.github/workflows/locks-render.yml`](.github/workflows/locks-render.yml)) — never use it as the authoritative read. Compute the intersection of the new slice's planned write set against every active claim returned by `locks-show.sh`. **Empty** → claim via `bash scripts/dev/lock-claim.sh <slug> <write-set-file>` (env vars `AGENT_ID`, `LOCK_BRANCH`, `LOCK_PLAN` populate `claim.json`; see `docs/design/git-ref-plan-locks.md` § Phase 1). **Non-empty** → STOP. Either coordinate with the holding slice (adjust scope / sequence behind it / pick a different slice) or escalate to the user with the overlap inventoried via `AskUserQuestion`. Every delegated packet must instruct the implementer with the literal sentence: *"Run `bash scripts/dev/locks-show.sh` first. Refuse if your write set overlaps any active claim; surface to the orchestrator. On scope growth, run `bash scripts/dev/lock-claim-update.sh <slug> <write-set-file>`."* Each PR holding a lock must include a `lock-slug: <slug>` line in its body so [`.github/workflows/lock-cleanup.yml`](.github/workflows/lock-cleanup.yml) auto-releases the ref on merge. Historical (pre-cutover) plan-lock entries live in [`docs/design/_plan-locks-archive.md`](docs/design/_plan-locks-archive.md) for audit-trail reference only — that file is frozen and no longer accepts new claims.
- **Owner + scope**: target agent, allowed write set, and files / modules that are explicitly out of scope.
- **Inline task context**: paste only the relevant design-doc section(s). Say "do not reopen the design doc unless blocked" when the excerpt is complete.
- **Shared inventory**: for exact symbol / literal work, do one exhaustive text-search in the orchestrator and pass matches as `<file>:<line>:<role>` (for example `(cfg-read)`, `(draft-write)`, `(audit-only)`). Do not make every agent rediscover the same inventory.
- **Invariant decisions**: scan the task against the hard rules in this file first. If the plan collides with `ITrackerClient`, command registry contracts, view storage, MCP schemas, dual-target constraints, or other invariants, pre-resolve the intended option in the prompt.
- **Subsystem split**: count the subsystem table rows touched. If a design-doc PR spans more than one subsystem row, split it before delegating unless a single cross-cutting design decision is still unresolved.
- **Output budget**: for routine implementation agents, request `Report <= 200 words, table form, no prose paragraphs` unless the task needs a design write-up.
- **Comment discipline**: remind implementation agents that code comments must explain durable code intent, never the task / PR / temporary plan (no comments like `PR 4:` or `remove in PR 7`).
- **Plan revision contract**: name the originating `docs/design/<slug>.md` in the packet and remind the implementer to append to `## Implementation log` + `## Deviations from plan` + `## Verification` in the same or next commit per AGENTS.md § Plan revision after implementation. Plans that ship without revision turn stale.
- **Verification automation handoff**: if the agent's report ends with any manual verification step ("user opens X and observes Y", "click and check"), the orchestrator's next move is **always** to invoke `test-author` to convert it. Implementing agents must explicitly list manual steps in their report so the orchestrator can dispatch automatically; "no manual steps" is also a valid statement.
- **File-split closure rule**: when delegating a multi-file split of a monolithic `.cpp` (`BlameAnalysisUi.cpp` shape), the packet must state the *closure rule* — "everything `<target-fn>` calls that isn't already in another TU goes to `<bucket>`" — not enumerate symbols from memory. Enumeration misses transitive callees (export builders, modal helpers); a closure rule does not.
- **Post-split include-replication rule**: after creating the shared internal header for a split, scan the original `.cpp`'s include list and replicate every non-self include into the internal header. Includes that were only in the original `.cpp` are silent until build — eliminate them up-front.
- **Plan-time production-file existence check**: before finalising any test-coverage plan (or any plan whose write set names specific production `.h` / `.cpp` files), the orchestrator runs a one-liner Glob / skeleton scan to confirm every named production file actually exists at the stated path. Five seconds of pre-flight catches plan / tree drift (renamed file, file moved during a refactor, name aliased an older path) that otherwise costs the delegated agent ~10 min of cross-walk per phase before deferral entries can be written confidently.
- **Subagent progress markers reminder** (per § Subagent progress markers): every delegation packet must include the literal sentence: *"Emit one-line progress markers to `.progress.log` via `bash scripts/dev/agent-progress.sh "<phase>: <text>"` at each major step (start, lock, design, code, test, gate, commit, push, pr, end) so `tail-agent.sh` shows live progress."*
- **Pure-helper TU-split recipe**: when a delegation targets a unit whose pure helpers sit in an anonymous namespace inside a `.cpp` whose top-of-file pulls banned deps (cpr / httplib / SQLite / ImGui / GLFW / Unreal headers), pre-authorise the production-side split in the packet. Allowed write set includes `Source_Core/{include,src}/<Unit>Pure.{h,cpp}` (or `<Unit>Parse.{h,cpp}` for parsers — Phase 1 `IssueCreatePipelineHelpers` + `P4BlameParse` shape). Recipe — extract pure helpers to the new TU under namespace `smatchet::<unit>::pure`; rewire call sites in the original `.cpp` via `using` decls inside the anon namespace; the new TU must have zero banned-header includes (verify with grep guard). Without this pre-authorisation, the agent's auto-mode classifier denies the split. Instances applied: `IssueCreatePipelineHelpers`, `P4BlameParse`, `McpJsonRpcPure`, `ILuaBindingHost` + `AppController_LuaBindingsCore`. Live instances awaiting the same lift: `IsTrackerTransportErrorText`, the 4 Phase-1-deferred tracker units (Labels / DateTime / Payload / Catalog) — see `docs/backlog/agent-self-improvement/infra.md`.

### Parallel dispatch

Run two or more subagents in a **single tool-use block** (multiple `Agent` calls in one message) when their contracts are independent. Examples:

- Symptom "slow AND wrong output" → `perf-detective` + `debug-detective` concurrently.
- Pre-merge gate → `code-review` + `security-review` concurrently.
- Multi-subsystem feature with disjoint write sets → multiple subsystem specialists at once.

**Do not parallelise when one delegation feeds another** (`architect` → subsystem agents). Sequential when contract-coupled; parallel otherwise. Wall-clock saved scales linearly with batch width; context isolation preserved per agent.

### Session scratchpad protocol

A per-session orchestrator scratchpad lives at `.session-context.md` at the repo root (gitignored). The `SessionStart` hook (`scripts/clear-session-context.sh`) archives the prior scratchpad (when it carries any agent-appended `## ` section) to `.session-context.archive/<ts>-<sid8>.md`, then writes a fresh banner. The `SubagentStop` hook (`agent-token-log.py`) appends a dated header block from each subagent whose report carries a `## Session context append` section.

Rules:

- **Subagents do not Read or Edit `.session-context.md` themselves.** The orchestrator reads it once per turn and passes relevant context inline to each subagent's prompt. This avoids races when subagents run in parallel and avoids duplicating the vexp `run_pipeline`-first rule.
- **Subagents emit `## Session context append`** in their report when there are session-durable facts worth surfacing — repro state, file:line evidence, decisions locked, open questions. Hook captures + appends; agent never writes the file.
- **Append-only.** Hook never edits prior entries.
- **Cross-session archive.** The SessionStart hook moves any non-trivial prior scratchpad to `.session-context.archive/` before truncating. Archives are gitignored, never auto-pruned (cheap on disk), and surfaced on demand by the `scratchpad-recall` skill — see [`agents/_shared/skills/scratchpad-recall/SKILL.md`](agents/_shared/skills/scratchpad-recall/SKILL.md). Use it when the user references "last session", "yesterday's run", "what did <agent> find earlier", or any cross-session continuity question.

Section shape:

```
## Session context append
- <fact 1 with file:line>
- <decision locked>
- <open question handed back to orchestrator>
```

### Subagent progress markers

Subagents dispatched in isolated worktrees emit single-line progress markers to `.progress.log` at the worktree root (gitignored). The orchestrator (and the user) tails the file via `bash scripts/dev/tail-agent.sh` to see live progress without parsing JSONL transcripts or polling `git status`.

**How** — one line per meaningful step via the helper:

```bash
bash scripts/dev/agent-progress.sh "<phase>: <one-line message>"
# or two-arg form:
bash scripts/dev/agent-progress.sh <phase> <one-line message>
```

The helper resolves the worktree root via `git rev-parse --show-toplevel`, appends a `[HH:MM:SS] <phase>: <message>` line, and flushes (each invocation is a separate process — the `>>` redirect closes on exit, so `tail -f` sees the line within ~1 s).

**When** — emit at these phase transitions, minimum:

| Phase | When to emit |
|---|---|
| `start` | First Bash call after entering the worktree |
| `lock` | After appending the plan-lock claim |
| `design` | Before writing the first production file (one line per major artifact: header drafted, .cpp scaffolded) |
| `code` | Each major implementation chunk landed (e.g. "AgentsMdLoader.cpp implemented") |
| `test` | Each new test TU drafted + each new test case clustering |
| `gate` | Before each `cmake --build` / `ctest` / `test-all.sh` invocation, plus result |
| `commit` | After `git commit` |
| `push` | After `git push` |
| `pr` | After `gh pr create` (include the PR URL on the same line) |
| `end` | Final line before producing the report |

**Why** — real-time visibility is otherwise expensive: the `<agentId>.output` stdout-capture file is empty for non-Bash agent work, and watching git-status snapshots in a loop is noisy. A small explicit log is low-cost to emit and gives the user a clean stream of "agent is at step N of M". Best-effort — not a merge gate; agents that forget aren't blocked but visibility regresses.

**Quoting note** — phase + message live on one bash arg; use shell-safe quoting (no embedded newlines; avoid `$` unless intentional). Anything with backticks or curly braces should be single-quoted.

### Tool-trace contract

Captured automatically — no agent burden. The `SubagentStop` hook counts `tool_use` blocks in the transcript and emits a `tool_trace` field on each JSONL row (e.g. `"Edit×4, Read×8, Bash×2"`). `scripts/agent-tokens-report.py` surfaces totals; `agents-statusline.py` shows top-agents by tokens. Agents may include an explicit `## Tool trace: ...` line in their report for the user's eye but the canonical count is hook-derived.

### Agent output contract

Agents fall into four classes by report shape. **Required section minimum** (extensions allowed):

| Class | Members | Required sections |
|---|---|---|
| **Investigator** (read-only diagnosis) | `architect`, `debug-detective`, `perf-detective`, `spike-hunter`, `code-review`, `security-review` | `## Hypotheses` (or `## Findings` for review agents) → `## Evidence` → `## Cause` (or severity-bucketed list) → `## Handoff` (target agent + allowed write set) |
| **Implementer** (read-edit subsystem) | `tracker-backend`, `grid-engine`, `offline-sync`, `command-system`, `lua-binder`, `mcp-toolsmith`, `p4-blame`, `unreal-bridge`, `mechanic` | `## Files changed` → `## Smoke-test result` → `## Manual residue` (must say "none" if none) |
| **Helper** (terminal helper) | `perf-instrument`, `perf-measure` | `## Spec executed` → `## Result` (numbers / inserted-or-stripped count) |
| **Maintenance** (workflow) | `build-doctor`, `test-author`, `git-janitor` | `## Pre-flight` → `## Mutations applied` → `## Regression gate` → `## Residue requiring user action` |

All four classes also end with `## Outcome: <state>` + `## Session context append` (when relevant) + `## Self-improvement` (per AGENTS.md § Self-improvement loop). `## Outcome:` value is one of `applied | halted | failed | partial | aborted` and is what the telemetry hook keys on.

### Trigger auto-activation

Orchestrator-side routing table — consulted **before** falling back to the heuristic block at the end of § Delegation:

| Keyword(s) in user prompt | Agent |
|---|---|
| slow, FPS, lag, profile, optimize | `perf-detective` |
| spike, hitch, freeze, stutter, intermittent, occasional hang | `spike-hunter` |
| crash, broken, regression, wrong output, doesn't work, assert, debug, investigate, "fix bug", "looks wrong" | `debug-detective` (**pause-loop, see § Debug-mode pause-loop**) |
| review, pre-merge, PR review, /review | `code-review` |
| security, vuln, secret, injection, audit, CVE | `security-review` |
| build, cmake, preset, link, packaging, lld, LTO | `build-doctor` |
| automate testing, manual verification, headless test | `test-author` |
| end of session, merge open PRs, tidy up, post-merge cleanup | `git-janitor` |
| stress-test plan, grill, interrogate | `grill-with-docs` (skill, not agent) |
| test, ctest, doctest, unit-test, SmatchetTests | `test-rig` |

Each per-agent `triggers:` frontmatter list mirrors its row plus agent-specific synonyms.

### Debug-mode pause-loop (overrides ship-loop)

When the user prompt matches the `debug-detective` trigger row above ("fix bug", "debug X", "investigate Y", "wrong output", "regression", "crash", "broken", "doesn't work", "looks wrong", "this used to work"), the orchestrator follows a **Cursor-style pause-loop** and **suspends the default autonomous ship-loop** (memory `feedback_autonomous_ship_loop.md`) for the duration of the investigation.

**Phases — every phase ends in an explicit pause:**

1. **Clarify** — orchestrator (or `debug-detective`) batches every uncertainty into one `AskUserQuestion` before the first mutating tool call. Front-loaded; no drip-feed mid-loop.
2. **Hypothesise** — write 2-4 falsifiable hypotheses ranked by distinguishing-evidence cost.
3. **Instrument** — temporary `[temp-debug]` markers + the NDJSON helper (`tests/_debug/SmatchetAgentDebug.h`). Both sides of the suspect boundary.
4. **Run** — auto-repro path when a CLI / scenario / Lua / doctest exists; otherwise **ask the user to reproduce** and supply the fresh-exe path + steps + log location.
5. **Read** — parse logs, mark each hypothesis confirmed / rejected / open.
6. **Pause + report** — emit the mid-loop report shape from `agents/debug-detective.md` § Report Shape, ending with an `AWAITING USER FEEDBACK` line. **Stop.** Orchestrator does **not** auto-commit, auto-push, or open a PR while waiting.
7. **Resume on user signal** — acceptable signals: "fixed", "still broken + here's the log", "try hypothesis N", "use this repro instead", "skip to handoff", "abort". Loop back to (2) for next-round or (3) directly when call sites are already chosen.
8. **Promote useful logs** — once the cause is pinned, promote ≤ 3 high-value boundary / state-transition logs to permanent (`LOG_DEBUG` / `LOG_INFO`). Zero promotions is valid.
9. **Strip temp** — `[temp-debug]` text-search must return zero hits. Delete the per-investigation helper + NDJSON log file.
10. **Hand off** — package the cause + write set + decision-resolved invariants + verification metric for the matching subsystem specialist. The fix-commit-push-PR-merge ship-loop **resumes from here**, owned by the subsystem specialist, not by `debug-detective`.

**Why a separate loop.** Bugs are an iterative narrowing problem. The default ship-loop ("diagnose → fix → build → commit → push → PR end-to-end without re-prompt") collapses the diagnose phase, ships the first plausible fix, and discards intermediate evidence. The pause-loop keeps each round's evidence in the user's hands and forbids speculative product edits until a hypothesis is confirmed.

**Pause-loop is not pause-everything.** Within a single round the agent freely edits instrumentation, builds, runs auto-repros, reads logs. The pause is at the **round boundary** — once `Read` + hypothesis-status update is done, the agent reports and stops.

### Skeleton-first

**Hard rule.** For files you're **inspecting** (understanding shape, finding the right symbol, scoping a change) use `get_skeleton` (or the harness equivalent — see § Harness adapter). For files you're **editing** use `Read`. Reading a full file for context-only inspection wastes ~70–90% of input tokens.

The split:

- "Where is X declared?" → skeleton
- "What's the shape of this dir?" → skeleton across all files
- "What does this function actually do?" → `Read` (but only that function, not the whole file)
- "I'm about to edit line N" → `Read`

Investigator agents (architect, code-review, security-review, debug-detective) are the heaviest readers and so the highest-ROI consumers of this rule.

### Agent versioning

Every agent carries a `version: <N>` integer in frontmatter. **Bump on**: capability tag added/removed, workflow contract changed (new mandatory section, new cleanup discipline), breaking output-shape change (renamed report section a downstream agent reads). **Don't bump on**: prose tweaks, typos, banner reformatting, token-efficiency tightens that preserve semantics. Telemetry (`agent_version` field on every JSONL row) lets `scripts/agent-tokens-report.py` flag the case where two versions of the same agent ran in one window — usually a mid-flight prompt edit.

### Cross-cutting

| Agent | Complexity · access | Use when |
|---|---|---|
| `architect` | high · read-only | Change spans `Source_Core` + `Plugins` (+ `UnrealPlugins`), or alters `ITrackerClient`, the command registry contract, per-backend view storage, or MCP schemas. Hand off **before** writing code — returns a design doc; the orchestrator implements. |
| `build-doctor` | high · read-edit | CMake / Ninja / MSYS2 / lld / LTO / `SmatchetPackageUnrealLibs_DX12` failures. Pass the preset name and the failing output verbatim. |
| `perf-detective` | high · read-only | Steady-state perf — optimize / profile / FPS / sustained lag. Owns hypothesis + diagnose + validate over frame averages. Delegates to `perf-instrument` and `perf-measure`. Wraps `docs/PERF_WORKFLOW.md`. |
| `spike-hunter` | high · read-only | Intermittent UI-thread stalls — spike / hitch / freeze / stutter / "occasionally slow". Looks at p99 / max outliers + blocking calls reaching the UI thread (HTTP, SQLite, p4, file I/O, locks). Delegates to `perf-instrument` and `perf-measure`. |
| `debug-detective` | high · read-edit | Behavioural bugs — crash / wrong output / regression / "broken" / "doesn't work". Inserts temporary `LOG_DEBUG` / `LOG_TRACE` markers (prefixed `[temp-debug]`), builds, runs via the unified CLI (`SmatchetStandalone.exe cmd …`), reads logs, proposes the cause; hands the fix to the matching subsystem specialist. Cleans up every `[temp-debug]` before claiming done. NOT for perf — that's `perf-detective` / `spike-hunter`. |
| `perf-instrument` | low · read-edit | Helper for `perf-detective` / `spike-hunter` — inserts / strips `SMATCHET_UI_PERF_SCOPE("perf_temp:…")` markers per spec, with overhead rules encoded. |
| `perf-measure` | low · read-only | Helper for `perf-detective` / `spike-hunter` — runs `perf.reset` → `scenario.run` → `perf.snapshot`, returns top-N rows by `lastTotalMs`. Standalone "what's hot right now" check also fine. |
| `code-review` | medium · read-only | Pre-merge code review. Runs cppcheck / clang-tidy / clang-format over the whole branch diff + Smatchet invariants. Wraps the standard pre-merge review skill. |
| `security-review` | high · read-only | Pre-merge security review. Runs flawfinder / semgrep / gitleaks (when available) + Smatchet attack-surface map. Wraps the standard pre-merge security skill. |
| `test-author` | medium · read-edit | Verification automation — converts every "user opens X and observes Y" plan item into a deterministic CLI / scenario / screenshot / sanitizer / ImGui Test Engine assertion. Invoke at plan time, after first verification round, and after every agent that hands back a manual step. Writes `scripts/dev/test-<feature>.sh`; the unified runner is `scripts/dev/test-all.sh`. Goal is zero manual steps. |
| `git-janitor` | medium · read-edit | End-of-session git maintenance — squash-merge open PRs in dependency order, delete merged branches, run regression build + `scripts/dev/test-all.sh` as the final gate. Refuses on uncommitted user work, force-push to develop/main, revert authoring, direct push to develop. Invoke after the last PR of a session lands and the user signals "no more changes coming". |
| `mechanic` | low · read-edit | Fully-specified mechanical work: renames, clang-format passes, doc / comment fixes, copyright bumps, localization key renames. Resolve ambiguity before delegating. |

### Subsystem specialists

| Agent | Complexity · access | Use when |
|---|---|---|
| `tracker-backend` | low · read-edit | `ITrackerClient`, `JiraClient`, `PlaneClient`, field catalog / value parser / payload, `TrackerHttpClient`, `IssueCreatePipeline`. Adding fields, fixing parsing, JQL / Plane queries, HTTP retries, audit-trail wiring. |
| `grid-engine` | low · read-edit | Spreadsheet / ticket grid — `TicketGridModel`, `SpreadsheetState`, `SmatchetActiveProjectGridUi`, all `SmatchetGrid*`, `SmatchetViewsDashboardUi*`, `SmatchetFieldRender`, `TrackerGridFieldDisplay`. Columns, cell editors, sorting, drag-reorder, header UX, in-place edit flow. |
| `offline-sync` | low · read-edit | SQLite cache, offline-queue replay, audit trail — `LocalCacheManager`, `OfflineQueueService`, `SmatchetOfflineQueueUi`, `TicketSyncService`, `BackendAuditTrail`, `FieldEditAuditSource`. Schema additions, replay, dead-letter, conflict resolution. |
| `command-system` | low · read-edit | Adding / modifying commands in the unified registry (CLI + Palette + MCP + Lua + Scenarios). Touches `Source_Core/{include,src}/Commands/`. |
| `lua-binder` | low · read-edit | sol2 bindings — `AppController_LuaBindings.cpp` ↔ `AppController_LuaStubs.cpp` sync, sandbox / timeout protection, `LuaAutomationHost`, `Plugins/LuaConsole`, hot-path cost trade-offs. |
| `mcp-toolsmith` | low · read-edit | `Plugins/Mcp/` + `SmatchetMcpServerUi` — MCP wire protocol, tool schemas (JSON-RPC), server lifecycle, REST envelope shape. |
| `p4-blame` | low · read-edit | Perforce blame — `P4Blame`, `P4ErrorUtil`, `BlameAnalysisUi`, `CppSyntaxHighlight`, `CallstackParser`. `p4 annotate` / `p4 describe`, blame caching, stack-frame symbolication via `PathRemaps`, Jira-comment export. |
| `unreal-bridge` | low · read-edit | Dual-target divergence — `SmatchetCore_DX12`, `UnrealPlugins/SmatchetImGuiPlugin`, `SMATCHET_EMBEDDED_IN_UNREAL`, header pollution in `Source_Core/`, packaging output. |
| `test-rig` | low · read-edit | Pure-logic doctest rig under `tests/` — `tests/CMakeLists.txt`, `tests/Source_Core/<Unit>.test.cpp`, `SMATCHET_BUILD_TESTS`, `ninja-test-msys2` preset. Adding tests for pure C++14 helpers (JQL surgery, value parsers, queue-replay decision math), expanding coverage, fixing wrong assertions. **Refuses** UI / HTTP / SQLite / ImGui / cpr surfaces — those route to bucket-E or stay deferred. |

### Stay in the orchestrator for

- Routine command registration (follow `RegisterCommand({...})` pattern; delegate to `command-system` only for non-trivial cases)
- Routine ImGui panels in `Source_Core`
- View-column additions
- Field-catalog tweaks
- `Locales/*.json` strings
- Perforce blame UI tweaks
- Additive SQLite schema changes
- Adding a single `CHECK` to an already-tested `tests/Source_Core/<Unit>.test.cpp` (delegate to `test-rig` only when scoping a NEW unit's test surface)

### Heuristic

- \>3 files across ≥2 top-level dirs **and** the design isn't obvious → `architect`
- Prompt already specifies file paths + symbols + commit messages → design is resolved, **skip `architect`**, go direct to the matching subsystem specialist or `mechanic`
- One symbol across many files → `mechanic`
- Symptom is "slow" / FPS / sustained lag → `perf-detective`
- Symptom is "occasional hang" / hitch / spike → `spike-hunter`
- Symptom is "crash" / "wrong output" / "regression" / "broken" / "doesn't work" → `debug-detective`
- Build / link / preset / packaging failure → `build-doctor`
- Change clearly sits inside one subsystem table row → that specialist
- Else the orchestrator handles it directly

### `delegates-to:` frontmatter

Optional. Agents that **directly call** another agent (helper-driven workflows like `perf-detective` → `perf-instrument` / `perf-measure`, or perf hand-offs from `code-review` / `grid-engine`) list those targets. Terminal agents (`mechanic`, `perf-instrument`, `perf-measure`, `build-doctor`, `command-system`, `p4-blame`) and agents that hand back to the orchestrator (`architect`, `security-review`, `tracker-backend`, `lua-binder`) omit the field. Absence ≠ "never delegates"; it means "delegation goes through the orchestrator, not direct."

### Why split

Each delegated agent gets a fresh context window — `tracker-backend` work doesn't load CMake helpers, `build-doctor` doesn't load `Source_Core/` headers, `perf-detective` doesn't load MCP schemas. That context isolation is the real token win, bigger than per-model price differences.

### Complexity rationale

`high` is reserved for one-shot-or-lose decisions (design, build root cause, perf root cause, security review). `medium` covers careful reading-and-flagging where mistakes are recoverable (`code-review`). Subsystem specialists run `low` because the invariants are stated up-front in their prompts — they apply patterns, they don't derive them. `mechanic` is `low` because pattern application doesn't benefit from deeper thinking and the diff is verifiable at a glance.

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
