# Agent self-improvement backlog

Suggestions emitted by delegated agents (canonical at `agents/`, mirrored to `.claude/agents/` for Claude Code auto-discovery) for improving the agent ecosystem itself — prompt tweaks, missing context, redundant steps, new-subagent candidates, tooling gaps.

The main thread appends new entries here (dedupe against existing). Periodically triage and apply real wins to agent prompts; close out items that landed by deleting them from this file (git history preserves them).

## Format

```
- YYYY-MM-DD · <agent-name> · [shortcut|process|tooling|context|new-agent] — one-line description
  Details: (optional, single paragraph or short bullet list — context that explains why this would help)
  Status: open | applied | rejected (with reason)
```

Categories:

- **shortcut** — a step the agent finds itself doing manually that could be encoded in its prompt as a default
- **process** — workflow friction: redundant steps, wrong order, missing handoff between agents
- **tooling** — missing CLI / static-analyzer / vexp invocation that would speed things up
- **context** — context the agent had to discover during the task that should be pre-loaded in its prompt
- **new-agent** — subsystem / task pattern that recurs and would warrant its own subagent

## Workflow

1. Delegated agents end every report with a `## Self-improvement` section. Empty is the common case and explicitly fine — agents only flag real friction.
2. The orchestrator reads the section, dedupes against this file, appends new entries with date + source agent + category.
3. When an entry has gathered enough evidence (mentioned by ≥ 2 agents, or blocks the same workflow ≥ 3 times), apply it: edit the relevant agent prompt(s) in `agents/`, regenerate the mirror via `scripts/sync-agents.sh`, and close out the entry.

## Triage cadence

Sweep the file when:

- Opening any PR that touches `agents/`
- The list exceeds ~20 open items

## Entries

<!-- Latest first. Append new entries at the top of this section. -->

- 2026-05-14 · lua-binder · [context] — sol2 v2.20.6 API limitations not in plan
  Details: Plans wrote `sol::this_state` first-param on usertype member functions and `sol::no_constructor` as a positional sentinel in `new_usertype(...)`. Both fail to compile with sol2 v2.20.6 (template `make_string_view` rejection on usertype-method signatures; no-constructor sentinel signature mismatch). Burned ~15 min of iteration. Add a hard invariant to `agents/lua-binder.md`: "sol2 v2.20.6 — recorder/usertype methods take plain args (no `sol::this_state` first param); `new_usertype` takes only `name` + method-name/ptr pairs, no constructor sentinel."
  Status: applied — agents/lua-binder.md § Hard invariants

- 2026-05-14 · lua-binder · [context] — Lua-as-C++ needs more than `LANGUAGE CXX`
  Details: Lua 5.3's `lua.h` lacks `extern "C"` guards, so flipping the source language to CXX mangles every Lua API symbol and the entire host fails to link. The plan for `lua-recorded-cmd-list` named only `set_source_files_properties(... LANGUAGE CXX)`; agent had to discover the second-step `luaconf.h` patch (or wrap host includes in `extern "C"`) the hard way. Add to `agents/lua-binder.md` hard invariants: "Compiling Lua 5.3 as C++ requires patching `luaconf.h` (or wrapping host-side `#include <lua.hpp>` blocks) with `extern \"C\"` — `LANGUAGE CXX` alone is insufficient."
  Status: applied — agents/lua-binder.md § Hard invariants

- 2026-05-14 · architect · [tooling] — `mcp__vexp__get_skeleton` empty result on indexed files
  Details: During plan-validation pass, `get_skeleton` returned "no skeleton data" for three files known to be in the vexp index. Forced fallback to `Read` for line-confirmation, adding 4 tool calls. Worth a pre-flight `index_status` health check before any read-only validation pass, or a guard in `agents/architect.md` to fall back automatically when skeleton returns empty for an indexed file.
  Status: applied — agents/architect.md § Pre-flight (fall back to Read on empty skeleton, optional index_status at start of long runs)

- 2026-05-14 · architect · [process] — `TodoWrite` reminder noise during read-only tasks
  Details: System injected three `TodoWrite` reminders into a read-only validation run. Read-only agents (architect, code-review, security-review, perf-measure) rarely benefit from a todo list; the reminder hook could be muted for them based on the agent banner or `tools:` frontmatter (no `Write`/`Edit`).
  Status: deferred — harness-side (Claude Code injects the reminder unconditionally; not configurable via project settings.json). Re-open once a Claude Code release exposes a per-agent toggle, or once a second harness cites the same noise.

- 2026-05-13 · p4-blame · [process] — multi-file split handoff packet missed transitive call closure
  Details: When splitting `BlameAnalysisUi.cpp` (2430 → 7 TUs), the orchestrator's handoff packet enumerated modal helpers explicitly but omitted export builders (`BuildAiExport`, `BuildBlameExport{Csv,Json}`, `BuildCallstackRowTsv`, `BuildAnnotatedRowTsv`) that `DrawWindow` calls. Agent had to discover them mid-split. Rule for orchestrator handoff packets on file-split tasks: include an explicit closure rule — "everything `<target-fn>` calls that isn't already in another TU goes to <bucket>" — instead of enumerating from memory.
  Status: open

- 2026-05-13 · p4-blame · [context] — missing-include after split is silent until build
  Details: When extracting helpers from a monolithic `.cpp` into separate TUs, includes that were only in the original `.cpp` are not catchable by inspection — only by build. `CompactDateFormat.h` was needed by both `_Config.cpp` and `_Window.cpp` after split but wasn't in the shared internal header. Rule for split-refactor agents: after creating the shared internal header, scan the original `.cpp`'s include list and replicate every non-self include into the internal header to pre-empt this class of failures.
  Status: open

- 2026-05-13 · orchestrator · [process] — branch-switch wipes untracked plan files
  Details: Working-tree-only files (e.g. a plan doc not yet `git add`-ed) are silently lost on `git checkout <other-branch>` when GitHub Desktop or `git reset --hard` runs. Recovery via `git fsck --lost-found` + content-search on dangling blobs is slow. Rule: as soon as a plan file lands at `docs/design/`, `git add` + commit it immediately, even with a `wip:` prefix, before any other work or branch operation. Never leave a plan file untracked across a session boundary.
  Status: applied (40c0bb2 — AGENTS.md § Project rules § Plan-doc safety)

- 2026-05-13 · orchestrator · [process] — wrong-exe testing burns iterations
  Details: When multiple build outputs exist (`build/ninja-release/`, `build/ninja-iter-msys2/`, worktree builds), the user can easily run the OLD exe and report bugs that the new patched exe doesn't have. Wasted ~5 round-trips this session. Rule: after each rebuild, `ls -la` both the patched and the most-likely-stale exe paths, print mtimes side-by-side, and tell the user explicitly which path to run. Same rule applies to subagents diagnosing perf or layout bugs.
  Status: applied (16eb7af — AGENTS.md § Debug techniques § Exe staleness check + agents/{perf-detective,spike-hunter,build-doctor}.md hard rules)

- 2026-05-13 · orchestrator · [process] — schema-version churn
  Details: The `kCurrentLayoutSchemaVersion` constant got bumped seven times in one session (1→2→3→4→5→6→7→8→9, then back to 2). Each bump shipped as its own commit because the user ran each intermediate fix and reported new symptoms. Rule: when a feature requires a config schema bump, hold the version bump until the feature is verified working end-to-end. Do not commit interim version bumps — squash or amend. Final version should be exactly one higher than the previous shipped version.
  Status: applied (40c0bb2 — AGENTS.md § Project rules § Schema-version bumps)

- 2026-05-13 · orchestrator · [shortcut] — pink-diagnostic clear color for UI gap detection
  Details: For "is the background ever visible behind panels?" questions: set `glClearColor(1.0f, 0.0f, 1.0f, 1.0f)` (or `ImVec4(1,0,1,1)` on DX12 RTV clear). Pink is rare in normal UI palettes; any visible pink is a guaranteed dock gap or transparent region. Combine with automated screenshot + per-pixel pink scan for objective regression tests (script template in this session's `debug.window.screenshot` PPM pipeline).
  Status: applied (40c0bb2 — AGENTS.md § Debug techniques § Pink-clear UI gap detection)

- 2026-05-13 · spike-hunter / orchestrator · [context] — ImGui docking state cannot be re-parented at runtime
  Details: `ImGui::LoadIniSettingsFromDisk()` after the first frame does NOT move already-created docked windows to new DockIds. A common bug pattern: schema migration runs post-load (in the per-frame Draw), windows stay at old positions. Rule: any dock-layout migration must run BEFORE `io.IniFilename` is set and the first `ImGui::NewFrame` call. In Smatchet this means inside `SmatchetImGuiHost::Initialize` (DX12) and inside `main.cpp` before `ImGui_ImplOpenGL3_Init` (Standalone).
  Status: applied (45c14c9 — agents/grid-engine.md + agents/unreal-bridge.md § Hard invariants)

- 2026-05-13 · code-review · [tooling] — lint hook does not run clang-format on newly created `.h` files
  Details: The `PostToolUse` hook via `.claude/hooks/lint-cpp.sh` runs clang-format on `.cpp` edits but not on new header files. New headers consistently arrive at code-review with alignment-padding violations that could have been auto-fixed. Add `*.h` (or `*.{cpp,h}`) to the hook's glob pattern in `.claude/settings.json` or the lint script.
  Status: open · investigation done, root cause NOT the filter
  Investigation (2026-05-13 session): repro confirmed by `Write`-ing a deliberately misformatted `.h` at `Source_Core/include/_lint_hook_probe.h`, then `Edit`-ing it; file stayed misformatted both times. Manual `bash .claude/hooks/lint-cpp.sh < <synthetic stdin>` invocation correctly formatted the file AND surfaced cppcheck output, so the hook script itself works. Filter at `.claude/hooks/lint-cpp.sh` L31-34 already includes `.h` files; `clang-format -i` runs unconditionally at L57. Real bug is upstream: Claude Code's `PostToolUse` matcher fires for `.cpp` files in this session but NOT for `.h` files via `Write`/`Edit` — or fires silently and the stderr does not surface. Backlog entry's proposed fix ("add `*.h` to glob") is therefore wrong — the glob is already correct. Next steps: (a) instrument the hook with a sentinel log file write to confirm whether it fires for `.h` Write/Edit, (b) check Claude Code's hook-discovery behaviour for `.h` paths (path filter, MIME / extension assumption?), (c) file upstream if confirmed harness-side. Not blocking; manual clang-format on touched `.h` files works.

- 2026-05-13 · architect · [process] — skip architect when prompt already specifies file paths + symbols + commit messages
  Details: Orchestrator dispatched a fully-specified three-commit implementation task (symbols, file paths, commit messages all pre-specified) to the `architect` agent (read-only design role). Architect cannot edit files or build. Rule of thumb to add to delegation heuristic: if the prompt already specifies file paths + symbols + commit messages, design is resolved — skip architect, go direct to general-purpose or the matching subsystem specialist.
  Status: applied (40c0bb2 — AGENTS.md § Heuristic, new bullet)

- 2026-05-12 · orchestrator · [process] — pre-resolve hard-invariant collisions before delegating implementation slices
  Details: The project-key run burned a full first attempt when a backend agent correctly paused on whether `ITrackerClient::FetchFieldCatalog` could be widened. The orchestrator already had AGENTS.md invariants in context; future delegation packets should state the approved option up front.
  Status: applied (d4714ad — AGENTS.md "Orchestrator delegation packet" § Invariant decisions; tracker-backend.md ITrackerClient widening rule hardened)

- 2026-05-12 · orchestrator · [context] — build one shared literal inventory and pass it to every delegated agent
  Details: Multiple agents rediscovered the same `cfg.ProjectKey` call sites. For exact symbol / literal work, do one exhaustive text-search in the orchestrator and pass `<file>:<line>:<role>` lines to each agent.
  Status: applied (d4714ad — AGENTS.md "Orchestrator delegation packet" § Shared inventory)

- 2026-05-12 · orchestrator · [context] — inline relevant design-doc sections in agent prompts and forbid rereads unless blocked
  Details: Agents reread the same long design doc even when each PR only needed a few sections. Paste the needed excerpt into the delegation packet and say whether reopening the source doc is allowed.
  Status: applied (d4714ad — AGENTS.md "Orchestrator delegation packet" § Inline task context)

- 2026-05-12 · orchestrator · [process] — cap routine implementation reports to short table form
  Details: PR 6 emitted a long prose report where a table of files / decisions / verification would have preserved the signal at much lower output cost. Default to `Report <= 200 words, table form, no prose paragraphs` for routine implementation agents.
  Status: applied (d4714ad — AGENTS.md "Orchestrator delegation packet" § Output budget)

- 2026-05-12 · orchestrator · [process] — remind subagents that code comments must not reference the task or PR plan
  Details: Several generated comments named PR numbers or temporary removal plans. Add the reminder to delegation packets because global comment discipline may not propagate reliably into delegated contexts.
  Status: applied (d4714ad — AGENTS.md "Orchestrator delegation packet" § Comment discipline)

- 2026-05-12 · orchestrator · [tooling] — allow text-search first for exhaustive literal / symbol inventories
  Details: vexp is best for ownership and impact, but graph-ranked semantic search is the wrong tool for "find every occurrence of this exact string." AGENTS.md should explicitly carve out mechanical inventory / rename / cleanup tasks.
  Status: applied (d4714ad + follow-up — AGENTS.md "Semantic-search exceptions" section, placed outside the auto-managed vexp block so it survives vexp tool updates)

- 2026-05-12 · orchestrator · [tooling] — dedupe or cap repeated lint-hook diagnostics from PostToolUse
  Details: The hook correctly runs after every C++ edit, but large multi-file changes can stream repeated analyzer output into every agent context. Consider per-file digesting, duplicate suppression, or a quiet mode that still surfaces real errors.
  Status: applied (d4714ad — `.claude/hooks/lint-cpp.sh` adds `format_issues` (awk dedupe + cap); `SMATCHET_LINT_MAX_LINES` env var, default 120, documented in `agents/build-doctor.md`)

- 2026-05-12 · command-system · [shortcut] — when the harness lint hook auto-runs on every edit, don't also run a batch `clang-format` at the end
  Details: Doing so produced large reformat diffs on `BuiltinCommands.cpp` / `PlaneClient.cpp` during PR 6 of the project-key removal. The PostToolUse hook in `.claude/settings.json` already covered every edited file.
  Status: applied (45c14c9 — agents/command-system.md § Hard invariants, new bullet)

- 2026-05-12 · grid-engine, command-system · [context] — localization accessor is `SmatchetLocalization::T(key, englishFallback)`, not `Loc(...)` / `Translate(...)`
  Details: Both PR 4b and PR 6 agents guessed wrong names and only converged via grep. Add a one-line note to `agents/grid-engine.md` and `agents/command-system.md`.
  Status: applied (d4714ad — agents/grid-engine.md L55 + agents/command-system.md L54 both carry the `SmatchetLocalization::T(key, englishFallback)` invariant)

- 2026-05-12 · grid-engine · [process / new-agent] — design-doc PRs that span ≥3 subsystems have no clear owner
  Details: PR 4 of the project-key removal touched tracker-backend (`ListProjects`) + grid-engine (draft picker, view pill) + bulk-import + i18n. `grid-engine` paused and asked for a split, which was correct but cost a round-trip. Either add an explicit `pr-driver` meta-agent that splits design-doc PRs into subsystem sub-delegations, or add a note to AGENTS.md instructing the orchestrator to pre-split such PRs before delegating.
  Status: applied (d4714ad — AGENTS.md § Orchestrator delegation packet § Subsystem split bullet covers pre-split rule; `pr-driver` meta-agent not pursued — orchestrator-side discipline is sufficient)

- 2026-05-12 · tracker-backend · [context] — `RemoteProject` POD uses lowerCamelCase (`id`, `key`, `displayName`) while most other DTOs in `Source_Core/include/` use PascalCase (`Id`, `Name`, …)
  Details: Style drift introduced in PR 1. Worth normalizing before more call sites accumulate. Architect call.
  Status: open
  Defer: C++ rename touching every `RemoteProject` call site (tracker-backend + grid-engine + bulk-import). Architect should scope the rename inside the next PR that legitimately touches `RemoteProject`. Don't open a standalone rename PR — bundle with adjacent work to minimise diff noise.

- 2026-05-12 · tracker-backend · [tooling / new-agent] — no test rig in the repo
  Details: pure-C++14 helpers (`JqlProjectScope`, value parser, JQL surgery) had to invent compile-only test patterns that aren't actually executed. A small `test-rig` agent that wires up a CTest target with doctest/GoogleTest against `Source_Core` would unblock real unit tests. High ROI given how much pure-logic code lives there.
  Status: open · plan scoped at `~/.claude/plans/test-rig-agent-shy-margulis.md` — covers doctest + CTest target + `agents/test-rig.md` design, locked scope decisions, and a 5-commit migration order. Close to `applied (<sha>)` once the rig actually lands.

- 2026-05-12 · tracker-backend · [context] — design-doc PR sections that list line numbers should mark each as `(cfg-read)` / `(draft-write)` / `(audit-only)`
  Details: Project-key PR 2 §2.3 listed lines 358, 382, 70, 92, 349 alongside `cfg.ProjectKey`-read sites, but they were draft-writes — required a disambiguation pass.
  Status: applied (d4714ad — AGENTS.md § Orchestrator delegation packet § Shared inventory mandates `<file>:<line>:<role>` with the exact role suffixes)

- 2026-05-12 · tracker-backend · [tooling] — `mcp__vexp__run_pipeline` rejects `max_tokens` as float when JSON wire format is double
  Details: Surfaced as "floating point, expected usize" — schema should accept integers-as-floats or improve the error message.
  Status: open
  Defer: External — vexp tool source lives outside this repo. File an issue / PR at the vexp project; not actionable in Smatchet. Workaround: cast to int literal in callers (`max_tokens: 12000` not `max_tokens: 12000.0`).

- 2026-05-12 · offline-sync · [shortcut] — `SaveFieldCatalogSnapshot` accumulated 4 extra primitive args; a `FieldCatalogSaveContext` struct would prevent future drift
  Details: callers already had each arg in scope; bundling them into one struct keeps the call site narrow as more per-axis state lands.
  Status: open
  Defer: Small C++ refactor — bundle into the next PR that touches `SaveFieldCatalogSnapshot`. Don't open a standalone refactor PR; the win shows up only when adding the next per-axis arg, which is when the bundling decision gets reviewed in context.

- 2026-05-12 · command-system · [process] — when a PR plan names a specific line/symbol, do a 30-second sanity grep before editing
  Details: Project-key PR 6 plan flagged `AppController_LuaBindings.cpp:~L254` as a "Lua config setter to deprecate" — it was actually `LuaApplyIssueCreateKv` (per-operation draft kv, not a config setter). One round-trip cost. Agent correctly flagged back to orchestrator before editing.
  Status: applied (45c14c9 — agents/command-system.md § Workflow step 3 + agents/tracker-backend.md § Workflow step 1)

- 2026-05-13 · orchestrator · [process] — vexp `<!-- vexp -->` block auto-regenerates inside `AGENTS.md`; should land in `.claude/CLAUDE.md` instead
  Details: AGENTS.md is the harness-agnostic root per the agents.md spec. The vexp tool injects ~30 lines of Claude-Code-specific MCP guidance (`run_pipeline`, `get_skeleton`, MCP tool list) directly into AGENTS.md, which other harnesses load and ignore. Editing the block in-place fights the regenerator. Upstream fix: vexp tool emits to `.claude/CLAUDE.md` only; the `@`-import in `.claude/CLAUDE.md` then pulls AGENTS.md without the vexp section.
  Status: open
  Defer: External — vexp tool source lives outside this repo. File an issue / PR at the vexp project. Workaround in the meantime: leave the block alone; the ~250 input-token cost per session is small relative to the auto-regen friction of fighting it.

- 2026-05-13 · orchestrator · [process] — ASCII em-dash banner bars (`━━━`) at agent open / close burn input + output tokens per call with no routing value
  Details: Every agent carried 4 `━` rules (open instr + open banner + close instr + close banner) plus the instruction prose ("Begin every response with this banner ... Use the horizontal rules"). ~9 lines × 18 agents = 162 lines of pure ceremony; emit cost is ~24 output tokens per subagent call. Banner content is per-agent unique only by `name` + `model/complexity/access`. Replaced with one-line banner spec: `**Banner** — open: \`🤖 AGENT: name · model/complexity · access\`. Close (before \`## Self-improvement\`): \`✅ END — name · model/complexity · access\`.`
  Status: applied (94d5836)

- 2026-05-13 · orchestrator · [context] — generic "Semantic search first" preamble duplicated across 11 agents; covered by AGENTS.md § Semantic codebase search
  Details: 11 of 18 agents carried near-identical `**Semantic search first** — call your harness's semantic codebase search ...` boilerplate. ~45 words × 11 = ~500 redundant input tokens permanently in agent corpus. Dropped from agents with no agent-specific guidance; kept on agents that add real twists (build-doctor's CMake file-read note, perf-detective / spike-hunter's `preset: debug` hint, perf-instrument / mechanic's exhaustive-text-search rule, code-review / security-review's process sections, perf-measure's CLI focus, debug-detective's Search Order). AGENTS.md § Semantic codebase search owns the canonical rule.
  Status: applied (94d5836)

- 2026-05-13 · debug-detective · [tooling] — NDJSON helper C++ template embedded inline in agent prompt (~85 lines)
  Details: Section 4a wrote the full `SmatchetAgentDebug.h` body inside the agent prompt, so every debug-detective invocation pulled the helper into the system prompt even when no instrumentation was needed. Externalized to `agents/_shared/templates/SmatchetAgentDebug.h.tmpl` with a one-line `sed`-based copy in the prompt. debug-detective.md shrunk 633 → 547 lines (−86); per-invocation input savings are larger since the helper body now loads only when materialised.
  Status: applied (d79a8fc)

- 2026-05-13 · orchestrator · [process] — `delegates-to:` frontmatter present on 8 of 18 agents with no documented rule
  Details: code-review, debug-detective, grid-engine, mcp-toolsmith, offline-sync, perf-detective, spike-hunter, unreal-bridge listed it; others omitted it. Field is informational (Claude Code does not consume it; orchestrators may). Audit showed allocation is intentional — agents that **directly call** another agent list targets; terminal agents (mechanic, perf-instrument, perf-measure, build-doctor, command-system, p4-blame) and orchestrator-hand-back agents (architect, security-review, tracker-backend, lua-binder) omit it. Documented in AGENTS.md § `delegates-to:` frontmatter — absence ≠ "never delegates" but "via orchestrator, not direct."
  Status: applied (d79a8fc)

- 2026-05-13 · orchestrator · [tooling] — `harness-hints.claude-code.tools:` line duplicates `capabilities:` list and goes unread by Claude Code (which parses top-level `tools:`)
  Details: ~104 chars per `tools:` line × 18 agents = ~470 tokens. Top-level `tools:` is what Claude Code actually consumes for permission restriction; the nested hint is informational only. AGENTS.md § Harness adapter is the canonical capability → tool mapping. Dropped the line everywhere; kept `model:` + `effort:` (real routing knobs).
  Status: applied (d6ba897)
