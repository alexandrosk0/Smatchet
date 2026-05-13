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
  Status: open

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
  Status: open

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
