## vexp <!-- vexp v2.0.12 -->

**MANDATORY: call `run_pipeline` first — never grep / glob / Read the codebase.**
vexp returns graph-ranked, pre-indexed context in a single call.

### Workflow
1. `run_pipeline({ task: "..." })` — ALWAYS FIRST. Auto-detects intent (debug / modify / refactor / explore). Returns capsule + impact + memory + file content.
2. Make targeted changes.
3. `run_pipeline` again only if context is insufficient.

### Tools
- `run_pipeline` — primary; auto-intent, file content included
- `get_skeleton` — compact file structure (prefer over Read)
- `index_status` — health + repo aliases
- `expand_vexp_ref` — expand V-REF placeholders

### Sub-agents
Always run `run_pipeline` first and pass the returned context into spawned agents. Don't let sub-agents search the codebase independently.

### Multi-repo
`run_pipeline` queries all indexed repos by default. Scope with `repos: ["alias"]`.
<!-- /vexp -->

# Delegation (Claude Code)

Default: stay in this thread (Sonnet) for routine work. Delegate to a subagent in `.claude/agents/` when the task matches.

### Cross-cutting

| Subagent | Model · effort | Use when |
|---|---|---|
| `architect` | Opus · high (read-only) | Change spans `Source_Core` + `Plugins` (+ `UnrealPlugins`), or alters `ITrackerClient`, the command registry contract, per-backend view storage, or MCP schemas. Hand off **before** writing code — returns a design doc, this thread implements. |
| `build-doctor` | Opus · high | CMake / Ninja / MSYS2 / lld / LTO / `SmatchetPackageUnrealLibs_DX12` failures. Pass the preset name and the failing output verbatim. |
| `perf-detective` | Opus · high | Steady-state perf — optimize / profile / FPS / sustained lag. Owns hypothesis + diagnose + validate over frame averages. Delegates to `perf-instrument` and `perf-measure`. Wraps `.claude/PERF_WORKFLOW.md`. |
| `spike-hunter` | Opus · high | Intermittent UI-thread stalls — spike / hitch / freeze / stutter / "occasionally slow". Looks at p99 / max outliers + blocking calls reaching the UI thread (HTTP, SQLite, p4, file I/O, locks). Delegates to `perf-instrument` and `perf-measure`. |
| `perf-instrument` | Haiku · low | Helper for `perf-detective` — inserts / strips `SMATCHET_UI_PERF_SCOPE("temp:…")` markers per spec, with overhead rules encoded. |
| `perf-measure` | Sonnet · low | Helper for `perf-detective` — runs `perf.reset` → `scenario.run` → `perf.snapshot`, returns top-N rows by `lastTotalMs`. Standalone "what's hot right now" check also fine. |
| `code-review` | Sonnet · high (read-only) | Pre-merge code review. Runs cppcheck / clang-tidy / clang-format over the whole branch diff + Smatchet invariants. Wraps `/review`. |
| `security-review` | Opus · high (read-only) | Pre-merge security review. Runs flawfinder / semgrep / gitleaks (when available) + Smatchet attack-surface map. Wraps `/security-review`. |
| `mechanic` | Haiku · low | Fully-specified mechanical work: renames, clang-format passes, doc / comment fixes, copyright bumps, localization key renames. Resolve ambiguity here first. |

### Subsystem specialists

| Subagent | Model · effort | Use when |
|---|---|---|
| `tracker-backend` | Sonnet · low | `ITrackerClient`, `JiraClient`, `PlaneClient`, field catalog / value parser / payload, `TrackerHttpClient`, `IssueCreatePipeline`. Adding fields, fixing parsing, JQL / Plane queries, HTTP retries, audit-trail wiring. |
| `grid-engine` | Sonnet · low | Spreadsheet / ticket grid — `TicketGridModel`, `SpreadsheetState`, `SmatchetActiveProjectGridUi`, all `SmatchetGrid*`, `SmatchetViewsDashboardUi*`, `SmatchetFieldRender`, `TrackerGridFieldDisplay`. Columns, cell editors, sorting, drag-reorder, header UX, in-place edit flow. |
| `offline-sync` | Sonnet · low | SQLite cache, offline-queue replay, audit trail — `LocalCacheManager`, `OfflineQueueService`, `SmatchetOfflineQueueUi`, `TicketSyncService`, `BackendAuditTrail`, `FieldEditAuditSource`. Schema additions, replay, dead-letter, conflict resolution. |
| `command-system` | Sonnet · low | Adding / modifying commands in the unified registry (CLI + Palette + MCP + Lua + Scenarios). Touches `Source_Core/{include,src}/Commands/`. |
| `lua-binder` | Sonnet · low | sol2 bindings — `AppController_LuaBindings.cpp` ↔ `AppController_LuaStubs.cpp` sync, sandbox / timeout protection, `LuaAutomationHost`, `Plugins/LuaConsole`, hot-path cost trade-offs. |
| `mcp-toolsmith` | Sonnet · low | `Plugins/Mcp/` + `SmatchetMcpServerUi` — MCP wire protocol, tool schemas (JSON-RPC), server lifecycle, REST envelope shape. |
| `p4-blame` | Sonnet · low | Perforce blame — `P4Blame`, `P4ErrorUtil`, `BlameAnalysisUi`, `BlameSyntaxHighlight`, `CallstackParser`. `p4 annotate` / `p4 describe`, blame caching, stack-frame symbolication via `PathRemaps`, Jira-comment export. |
| `unreal-bridge` | Sonnet · low | Dual-target divergence — `SmatchetCore_DX12`, `UnrealPlugins/SmatchetImGuiPlugin`, `SMATCHET_EMBEDDED_IN_UNREAL`, header pollution in `Source_Core/`, packaging output. |

### Stay in-thread for

Registering a command (follow the `RegisterCommand({...})` pattern with `command-system` only for non-trivial cases), routine ImGui panels in `Source_Core`, view-column additions, field-catalog tweaks, `Locales/*.json` strings, Perforce blame UI, additive SQLite schema changes.

### Heuristic

- \>3 files across ≥2 top-level dirs **and** the design isn't obvious → `architect`
- One symbol across many files → `mechanic`
- Symptom is "slow" or any FPS / hitch word → `perf-detective`
- Build / link / preset / packaging failure → `build-doctor`
- Change clearly sits inside one subsystem table row → that specialist
- Else stay here

### Why split

Each subagent gets a fresh context window — `tracker-backend` work doesn't load CMake helpers, `build-doctor` doesn't load `Source_Core/` headers, `perf-detective` doesn't load MCP schemas. That context isolation is the real token win, bigger than per-model price differences.

### Effort rationale

`high` is reserved for one-shot-or-lose decisions (design, build root cause, perf root cause). Subsystem specialists run `low` because the invariants are stated up-front in their prompts — they apply patterns, they don't derive them. `mechanic` is `low` because pattern application doesn't benefit from deeper thinking and the diff is verifiable at a glance.

## Self-improvement loop

Every subagent ends its report with a `## Self-improvement` section listing observations about the agent ecosystem itself — shortcuts, missing context that wasted a tool call, redundant steps, new-agent candidates, tooling gaps. **Empty is the common case and explicitly fine** — agents only flag real friction, never make up suggestions.

The main thread reads each agent's section, dedupes against [`backlog/AGENT_SELF_IMPROVEMENT.md`](backlog/AGENT_SELF_IMPROVEMENT.md), and appends new entries with date + source agent + category (`shortcut` / `process` / `tooling` / `context` / `new-agent`). See the backlog file for format and triage rules.

Apply entries when one has been mentioned by ≥ 2 agents or has blocked the same workflow ≥ 3 times. The goal is a self-tightening loop — agents notice friction, the main thread accumulates evidence, prompts get patched, friction drops.
