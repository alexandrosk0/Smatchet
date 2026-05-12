# Smatchet — agent + project rules

This is the canonical entry-point doc for any agentic harness (Claude Code, Codex / OpenAI Agents, Cursor, Aider, generic). Everything an agent needs lives in this repo — no external dependencies on user-global or parent-dir config.

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

**Lint**: your harness may run an automatic lint pass after C++ edits. Claude Code does so via the `PostToolUse` hook in `.claude/settings.json` calling `.claude/hooks/lint-cpp.sh` — `clang-format -i` applies in place; `cppcheck` + `clang-tidy` report to stderr. If your harness lacks hook automation, run those three tools manually on every edited `.cpp` / `.h` in `Source_Core` / `Plugins` / `Target_Standalone` and fix all reported issues before responding.

**Perf workflow**: when the user asks to optimize / profile / fix FPS / lag / hitch / "slow" / spike, read [`docs/PERF_WORKFLOW.md`](docs/PERF_WORKFLOW.md) and follow it. Don't load it for unrelated tasks.

## Semantic codebase search — use it first

Every agent in this repo expects the orchestrator (or the agent itself) to use **semantic codebase search** before falling back to text-search. In practice that means:

- **Always** call the harness's indexed codebase search first for any "where is X" / "what calls Y" / "what does this touch" question. This is faster, cheaper, and more accurate than raw `grep` over a multi-MLOC codebase.
- Prefer **compact file-skeleton views** (signatures + classes only) for files you're inspecting but not editing — typically 70–90% token savings vs full reads.
- Fall back to text-search + full reads only when no semantic search is available or its index is degraded.

Under Claude Code this maps to `mcp__vexp__run_pipeline` (semantic search) and `mcp__vexp__get_skeleton` (skeleton). Other harnesses substitute their equivalents (see the Harness adapter table below). Agents whose prose mentions vexp do so as a concrete example — the capability is what matters.

## Agent file locations

Agent definitions are **dual-located**:

- **Canonical**: `agents/<name>.md` at repo root. This is where humans edit and where the agnostic Agents.md spec ([agents.md](https://agents.md/)) places them.
- **Mirror**: `.claude/agents/<name>.md` — auto-generated copy for Claude Code's hardcoded discovery path. **Do not edit the mirror directly** — your changes will be silently overwritten on the next sync. Each mirror file starts with a YAML-comment warning to that effect.

After editing any file under `agents/`, run `bash scripts/sync-agents.sh` (or `scripts/sync-agents.ps1` on PowerShell-only Windows boxes) to refresh the mirror. The drift check `scripts/check-agents-mirror.sh` verifies they match (CI-friendly).

Harnesses other than Claude Code should read from `agents/` and ignore the `.claude/` mirror.

## Delegation

Default: stay in the orchestrator's primary model for routine work. Delegate to an agent in `agents/` when the task matches.

### Cross-cutting

| Agent | Complexity · access | Use when |
|---|---|---|
| `architect` | high · read-only | Change spans `Source_Core` + `Plugins` (+ `UnrealPlugins`), or alters `ITrackerClient`, the command registry contract, per-backend view storage, or MCP schemas. Hand off **before** writing code — returns a design doc; the orchestrator implements. |
| `build-doctor` | high · read-edit | CMake / Ninja / MSYS2 / lld / LTO / `SmatchetPackageUnrealLibs_DX12` failures. Pass the preset name and the failing output verbatim. |
| `perf-detective` | high · read-only | Steady-state perf — optimize / profile / FPS / sustained lag. Owns hypothesis + diagnose + validate over frame averages. Delegates to `perf-instrument` and `perf-measure`. Wraps `docs/PERF_WORKFLOW.md`. |
| `spike-hunter` | high · read-only | Intermittent UI-thread stalls — spike / hitch / freeze / stutter / "occasionally slow". Looks at p99 / max outliers + blocking calls reaching the UI thread (HTTP, SQLite, p4, file I/O, locks). Delegates to `perf-instrument` and `perf-measure`. |
| `perf-instrument` | low · read-edit | Helper for `perf-detective` / `spike-hunter` — inserts / strips `SMATCHET_UI_PERF_SCOPE("temp:…")` markers per spec, with overhead rules encoded. |
| `perf-measure` | low · read-only | Helper for `perf-detective` / `spike-hunter` — runs `perf.reset` → `scenario.run` → `perf.snapshot`, returns top-N rows by `lastTotalMs`. Standalone "what's hot right now" check also fine. |
| `code-review` | medium · read-only | Pre-merge code review. Runs cppcheck / clang-tidy / clang-format over the whole branch diff + Smatchet invariants. Wraps the standard pre-merge review skill. |
| `security-review` | high · read-only | Pre-merge security review. Runs flawfinder / semgrep / gitleaks (when available) + Smatchet attack-surface map. Wraps the standard pre-merge security skill. |
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
| `p4-blame` | low · read-edit | Perforce blame — `P4Blame`, `P4ErrorUtil`, `BlameAnalysisUi`, `BlameSyntaxHighlight`, `CallstackParser`. `p4 annotate` / `p4 describe`, blame caching, stack-frame symbolication via `PathRemaps`, Jira-comment export. |
| `unreal-bridge` | low · read-edit | Dual-target divergence — `SmatchetCore_DX12`, `UnrealPlugins/SmatchetImGuiPlugin`, `SMATCHET_EMBEDDED_IN_UNREAL`, header pollution in `Source_Core/`, packaging output. |

### Stay in the orchestrator for

Registering a routine command (follow the `RegisterCommand({...})` pattern with `command-system` only for non-trivial cases), routine ImGui panels in `Source_Core`, view-column additions, field-catalog tweaks, `Locales/*.json` strings, Perforce blame UI tweaks, additive SQLite schema changes.

### Heuristic

- \>3 files across ≥2 top-level dirs **and** the design isn't obvious → `architect`
- One symbol across many files → `mechanic`
- Symptom is "slow" / FPS / sustained lag → `perf-detective`
- Symptom is "occasional hang" / hitch / spike → `spike-hunter`
- Build / link / preset / packaging failure → `build-doctor`
- Change clearly sits inside one subsystem table row → that specialist
- Else the orchestrator handles it directly

### Why split

Each delegated agent gets a fresh context window — `tracker-backend` work doesn't load CMake helpers, `build-doctor` doesn't load `Source_Core/` headers, `perf-detective` doesn't load MCP schemas. That context isolation is the real token win, bigger than per-model price differences.

### Complexity rationale

`high` is reserved for one-shot-or-lose decisions (design, build root cause, perf root cause, security review). `medium` covers careful reading-and-flagging where mistakes are recoverable (`code-review`). Subsystem specialists run `low` because the invariants are stated up-front in their prompts — they apply patterns, they don't derive them. `mechanic` is `low` because pattern application doesn't benefit from deeper thinking and the diff is verifiable at a glance.

## Self-improvement loop

Every delegated agent ends its report with a `## Self-improvement` section listing observations about the agent ecosystem itself — shortcuts, missing context that wasted a tool call, redundant steps, new-agent candidates, tooling gaps. **Empty is the common case and explicitly fine** — agents only flag real friction, never make up suggestions.

The orchestrator reads each agent's section, dedupes against [`backlog/AGENT_SELF_IMPROVEMENT.md`](backlog/AGENT_SELF_IMPROVEMENT.md), and appends new entries with date + source agent + category (`shortcut` / `process` / `tooling` / `context` / `new-agent`). See the backlog file for format and triage rules.

Apply entries when one has been mentioned by ≥ 2 agents or has blocked the same workflow ≥ 3 times. The goal is a self-tightening loop — agents notice friction, the orchestrator accumulates evidence, prompts get patched, friction drops.

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

- **Claude Code** reads `.claude/agents/` automatically (use the mirror — run `scripts/sync-agents.sh` after editing canonical files). Also reads `@`-included files in `CLAUDE.md`.
- **Codex / OpenAI Agents** reads `AGENTS.md` per the [agents.md spec](https://agents.md/). Discover individual agents at `agents/*.md`.
- **Cursor / Aider / generic** — human-driven. Agent files at `agents/` are reference docs the user pastes or invokes manually.
- Harnesses without `semantic-code-search` should fall back to text-search with the symbol set named in each agent's prose. Output is degraded but workable — expect more round-trips and larger context per query.

**Per-agent harness hints**: each agent's YAML frontmatter may carry a `harness-hints.<harness>:` block with harness-specific routing details (e.g. Anthropic model selection, MCP tool list). Harnesses ignore unknown blocks.
