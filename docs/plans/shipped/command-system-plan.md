# Smatchet — Unified Command System (CLI + Palette + MCP + Lua)
<!-- index-summary: Unified Command System (CLI + Palette + MCP + Lua + Scenarios). Originally `backlog/COMMAND_SYSTEM_PLAN.md`. C++ source comments throughout `Source_Core/src/Commands/` + `Target_Standalone/` reference this. -->

## Context

The user asked to automate the perf workflow (`PERF_WORKFLOW.md`) so Claude can drive measurements without the user running the app, opening menus, and pasting numbers back. Today there's no automation hook: the CLI accepts only 5 config flags, MCP exposes only 3 fixed tools, and Lua bindings are read-only-ish helpers — none can dispatch arbitrary AppController operations or drive scenarios.

The user expanded scope: build a **modern CLI for testing + AI-agentic use, exposing all possible commands, also usable from inside the app via a Command Palette**.

A key user insight (better than my original headless proposal): **if a Smatchet instance is already running, the CLI should call into it rather than spawning its own headless instance**. The existing MCP HTTP server (cpp-httplib, configurable port) is the natural transport — no headless infrastructure needed. The interactive dev workflow always has the app running anyway, and CI can spawn an ephemeral instance via a `--spawn` flag.

**Outcome:** one command registry feeds four frontends — CLI sub-commands, in-app Command Palette (Ctrl+Shift+P), MCP tools (auto-exposed), Lua `commands.invoke`. Adding a new command means one `RegisterCommand({...})` call and it appears everywhere automatically.

## AI-CLI design principles (load-bearing)

The CLI must be agent-friendly first, human-friendly second. Every design decision below traces to one of these principles — re-check during implementation:

1. **Never interactive.** No `[y/n]` prompts, no `Read-Host`, no waiting for input. Missing required arg → exit non-zero with structured error explaining which arg + showing schema. Destructive ops require explicit `--yes`; absence is an error, not a prompt.
2. **Structured output, default JSON.** Stdout is JSON. Exactly one JSON document per invocation. `--pretty` indents it; `--quiet`/`-q` outputs bare values one-per-line for `xargs`-style pipes; `--format=table` renders human-friendly text (only for interactive terminals — never the default).
3. **Stdout = data, stderr = diagnostics.** Logs, progress, warnings → stderr. Stdout never mixes formats. A successful command's stdout is parseable JSON even if `--quiet` is on (then it's NDJSON or bare scalars).
4. **Descriptive naming.** Verbs are explicit (`sync.incremental` not `sync.run`; `tickets.list` not `tickets.fetch`). One verb = one operation, no flag-toggled mode-switches inside a command.
5. **Fail loud, fail structured.** Errors are JSON `{ok:false, error:{code, message, hint, suggestions:[...]}}`. `code` is a stable kebab-case enum (`unknown-command`, `missing-required-arg`, `validation-error`, `handler-error`, `confirm-required`, `not-connected`, `dry-run-unsupported`, `timeout`, …). `suggestions` for `unknown-command` returns the top 3 fuzzy matches.
6. **Self-describing.** `cmd <name> --help` and `cmd commands.help --name=<name>` both return the full schema + 1-2 example invocations. `cmd commands.list --json` is the agent's discovery entry point.
7. **Composable.** `--quiet` extracts the primary key (id/name) for pipe-chaining. List commands accept `--limit` + `--offset` (or `--cursor` for streamed). Numeric exit codes are stable (see CLI section).
8. **Token-aware.** `--tokens` flag estimates the output size in tokens (rough: `chars / 4`) and prints `{tokens_estimate, bytes}` to stderr without producing the data — so agents can decide whether to materialize a large response.
9. **Idempotency hints.** `Command.Idempotent: bool` field. Returned in `commands.help` so agents know which commands are safe to retry on transient failure.
10. **Stable contract.** Once a command name + schema ships, it's API. Renames go through alias tables (same mechanism as `list_active_tickets` → `tickets.list_active`). Schema changes are additive (new optional params with defaults) until a major version bump.
11. **Dry-run for destructive ops.** Every destructive command honors `--dry-run`: handler runs validation + computes the diff (`wouldDo`/`wouldChange`) but never mutates. Combined with `--yes` for real execution, this gives agents a safe two-phase loop (preview → confirm → execute). Commands advertise support via `Command.DryRunSupported`; commands without dry-run support return `dry-run-unsupported` if asked.
12. **Env-vars for ambient config and secrets.** Sensitive values (tracker tokens) and ambient runtime config (port, user-data dir) read from a stable set of `SMATCHET_*` env vars. Argv is for per-call params only — never secrets. Env names are part of the API contract; see "Environment contract" below.

### Feature-gated builds (`SMATCHET_WITH_*`)

Per [`docs/adr/0010-light-profile-feature-gated-command-registry.md`](../../adr/0010-light-profile-feature-gated-command-registry.md):

- When an optional feature is **OFF** at compile time (AI, MCP, Whisper, …), its commands are **not registered** — no stub handlers that return “disabled in this build.”
- `commands.list`, palette fuzzy search, and MCP `tools/call` only expose **present** features. Calling a disabled-feature name → **`unknown-command`** (+ fuzzy `suggestions` among registered names).
- Agents/scripts that need `ai.*`, MCP tools, or `whisper.*` must use **full** `Smatchet.exe` (MCP ON), not light/Unreal embed builds.
- **`Smatchet-Light.exe cmd …`** (MCP OFF): full CLI for **registered** light-profile commands via **in-process dispatch** — headless boot, `CommandRegistry::Dispatch`, JSON stdout. No MCP attach. Plan: [`light-release-unreal-default.md`](../light-release-unreal-default.md) grill Q1 revision (2026-05-24).
- Lua may still call always-on `AppController` no-op stubs for AI context helpers when AI is OFF; that is **not** the same as registry-visible `ai.*` commands.

---

## Architecture (single mega-PR per user choice)

### Module layout

**Source_Core (compiles in both Standalone + Unreal DX12 — no GLFW/OpenGL deps in registry):**
- `Source_Core/include/Commands/Command.h` — `Command`, `CommandResult`, `CommandContext`, `ParamSpec` types.
- `Source_Core/include/Commands/CommandRegistry.h` — registry singleton interface.
- `Source_Core/src/Commands/CommandRegistry.cpp` — thread-safe map, recents ring, dispatch with arg validation/coercion/defaults.
- `Source_Core/src/Commands/BuiltinCommands.cpp` — registers ~55 commands wrapping `AppController` API.
- `Source_Core/include/Commands/FuzzyMatch.h` + `.cpp` — Sublime-style subsequence scorer (no helper exists in repo; needs building).
- `Source_Core/include/Commands/CommandPaletteUi.h` + `Source_Core/src/Commands/CommandPaletteUi.cpp` — Ctrl+Shift+P modal. Header includes only ImGui (linked to both targets); no GLFW/OpenGL/Win32 includes — DX12 build must compile this file unchanged.
- `Source_Core/include/Commands/Scenarios/IScenario.h` — scenario base + `ScenarioRunner`.
- `Source_Core/src/Commands/Scenarios/PriorityGridScrollScenario.cpp` — first scenario.
- `Source_Core/src/Commands/PerfDump.cpp` — `UiPerfMonitor::GetLastFrameRows()` → JSON.

**Touched (existing):**
- `Source_Core/include/AppController.h` — add `CommandRegistry& Commands()`, `ScenarioRunner& Scenarios()`, `void TickActiveScenario()`, `bool ShouldExitFromScenario()`.
- `Source_Core/src/AppController.cpp` — own the registry, call `RegisterBuiltinCommands(*this)` at end of `Initialize()`.
- `Source_Core/src/AppController_LuaBindings.cpp` — add `commands.invoke(name, args_table)` Lua call. `ui.register_global_action` becomes a thin wrapper around `Commands().Register(...)` with `category="lua"`.
- `Source_Core/src/SmatchetUI.cpp` — Ctrl+Shift+P poll at top of `Draw()`, render `CommandPaletteUi`, call `app.TickActiveScenario()` once per frame.

**Standalone-only:**
- `Target_Standalone/CliCommandRunner.{h,cpp}` (new) — extract arg parsing from `main.cpp`, add `cmd <name>` subcommand, HTTP client to MCP endpoint, optional `--spawn` for CI.
- `Target_Standalone/main.cpp` — refactor to call `CliCommandRunner::Parse(...)`; if `cmd` token present and no `--spawn`, run as HTTP client (no app init); otherwise normal app boot. `--spawn` mode boots app, dispatches command in-process, quits on result.

**Plugin-only:**
- `Plugins/Mcp/McpPlugin.cpp` — `/mcp/tools/list` merges `app->Commands().All()` with existing built-ins. `/mcp/tools/call` checks registry first, falls through to today's logic. Old tool names (`list_active_tickets`, `search_active_tickets`) become aliases pointing at the new `tickets.list_active` / `tickets.search_active` registry entries.

### Core types (C++14 only — no `string_view`/`optional`/`variant`)

```cpp
// Command.h
namespace smatchet { namespace cmd {

enum class ParamType { String, Int, Bool, Number, Json };

struct ParamSpec {
    std::string Name;
    ParamType   Type = ParamType::String;
    bool        Required = false;
    std::string Description;
    nlohmann::json Default;          // null = no default
    std::vector<std::string> Enum;   // optional restriction
};

enum class ErrorCode {
    None,
    UnknownCommand,        // command name not in registry
    MissingRequiredArg,    // schema validation: required param absent
    ValidationError,       // schema validation: type mismatch, enum violation
    HandlerError,          // handler returned an error
    ConfirmRequired,       // destructive without --yes / __confirm
    NotConnected,          // CLI attach mode: no running app
    AppendOnly,            // attempted to mutate read-only state
    NotFound,              // ticket / view / field not found
    BackendError,          // tracker (Jira/Plane) returned non-2xx
    DryRunUnsupported,     // --dry-run passed to a command without Command.DryRunSupported
    Timeout                // spawn-mode async wait exceeded --timeout
};
const char* ErrorCodeString(ErrorCode c);  // stable kebab-case strings

struct CommandError {
    ErrorCode Code = ErrorCode::None;
    std::string Message;            // human-readable summary
    std::string Hint;               // actionable next step ("retry with --yes")
    std::vector<std::string> Suggestions;  // e.g. fuzzy did-you-mean matches
    nlohmann::json Details;         // optional structured detail (param name, etc.)
    nlohmann::json ToJson() const;
};

struct CommandResult {
    bool Ok = true;
    CommandError Error;            // populated when !Ok
    nlohmann::json Data;
    static CommandResult Success(nlohmann::json d);
    static CommandResult Failure(ErrorCode code, std::string message,
                                 std::string hint = {},
                                 std::vector<std::string> suggestions = {});
    nlohmann::json ToWireJson() const;  // canonical JSON output shape (see below)
};

enum class CommandSource { Cli, Palette, Mcp, Lua, Internal };

struct CommandContext {
    AppController* App = nullptr;
    CommandSource  Source = CommandSource::Internal;
    bool           ConfirmedDestructive = false;
    bool           DryRun = false;                  // set by --dry-run / __dry_run; handler must not mutate
    int            TimeoutMs = 0;                   // 0 = no cap; spawn-mode async wait ceiling
};

struct Command {
    std::string Name;          // dotted: "tickets.search_active"
    std::string Category;      // "tickets" | "view" | "perf" | "scenario" | "app" ...
    std::string Summary;       // one-line, agent-readable verb-first
    std::string Description;   // multi-line. Must include: returns shape, side effects, 1-2 example invocations
    std::vector<ParamSpec> Params;
    bool Destructive      = false;   // requires --yes / __confirm
    bool Idempotent       = true;    // safe to retry on transient failure (returned in commands.help)
    bool AsyncSafe        = true;    // false => must run on UI thread next-tick AND completes asynchronously (spawn-mode driver waits on PendingAsyncResult() future)
    bool DryRunSupported  = false;   // honors ctx.DryRun by computing diff without mutating
    std::vector<std::string> Aliases;  // back-compat: e.g. "list_active_tickets" -> "tickets.list_active"
    std::function<CommandResult(const nlohmann::json&, CommandContext&)> Handler;

    nlohmann::json BuildJsonSchema() const;   // for MCP inputSchema + commands.help
    std::string BuildHelpText() const;         // for `cmd <name> --help` (text, to stderr-or-stdout)
};

}}
```

`CommandRegistry` exposes `Register / Has / Find / All / ByCategory / Dispatch / Recents / FuzzyMatch`. One instance lives on `AppController`. **Reentrancy contract:** `Dispatch` copies the `Command` struct (including its `Handler` `std::function`) under a `std::mutex`, then releases the lock before invoking the handler — so a Lua handler that recurses back into `commands.invoke` can take the lock again without deadlock. **Guard order inside `Dispatch`:** (1) resolve aliases via `Find`; (2) `unknown-command` if not found; (3) param validation/coercion → `missing-required-arg` / `validation-error`; (4) if `Destructive && !ctx.ConfirmedDestructive && !ctx.DryRun` → `confirm-required` (dry-run intentionally bypasses confirm so agents can preview destructive ops); (5) if `ctx.DryRun && !DryRunSupported` → `dry-run-unsupported`; (6) invoke handler. `Find(name)` resolves aliases (e.g. `list_active_tickets` → `tickets.list_active`).

### Canonical wire JSON shape

Every CLI/MCP/Lua dispatch returns this shape — one JSON document, stable contract:

```json
{
  "ok": true,
  "command": "tickets.search_active",
  "data": { "matches": [...] }
}
```

```json
{
  "ok": false,
  "command": "tickets.search_actiev",
  "error": {
    "code": "unknown-command",
    "message": "No command named 'tickets.search_actiev'.",
    "hint": "Did you mean 'tickets.search_active'?",
    "suggestions": ["tickets.search_active", "tickets.list_active", "tickets.get"]
  }
}
```

Stdout is always exactly one such document (or an NDJSON stream for `--stream` commands like long-running scenarios). Logs / progress / warnings → stderr only. The one explicit exception is `cmd <name> --help`, which prints human-readable text to stdout (machine-readable help comes from `cmd commands.help --name=<n>`, which returns the canonical JSON envelope).

Dry-run shape adds a `dryRun:true` marker and a `wouldDo` payload describing the intended mutation without performing it:

```json
{
  "ok": true,
  "command": "ticket.set_field",
  "dryRun": true,
  "data": {
    "wouldDo": {
      "ticket": "X-1",
      "field": "status",
      "from": "In Progress",
      "to": "Done"
    }
  }
}
```

---

## Environment contract

These env vars are stable API surface — naming changes are breaking.

| Variable | Purpose | Default |
|---|---|---|
| `SMATCHET_MCP_PORT` | Override `instance.json` discovery for CLI attach | (read `instance.json`) |
| `SMATCHET_MCP_HOST` | MCP host for CLI attach (loopback only unless `--mcp-allow-remote` agrees) | `127.0.0.1` |
| `SMATCHET_USER_DATA` | Override `ConfigManager::GetUserDataDirectory()` | platform default |
| `SMATCHET_LOG_LEVEL` | One of `TRACE\|DEBUG\|INFO\|WARN\|ERROR` for CLI mode | `WARN` |
| `SMATCHET_NO_COLOR` / `NO_COLOR` | Suppress ANSI in `--format=table` | (auto-detect TTY) |
| `SMATCHET_DEFAULT_FORMAT` | `json` (default) or `table` for interactive shells | `json` |
| `SMATCHET_TRACKER_TOKEN` | Tracker (Jira/Plane) bearer token for `--spawn` mode | (read from config) |
| `SMATCHET_TRACKER_BASE_URL` | Tracker base URL for `--spawn` mode | (read from config) |
| `SMATCHET_SPAWN_TIMEOUT_MS` | Default wait ceiling for spawn-mode async commands | `120000` |

**Precedence**: explicit flag > env var > config file > built-in default. Env-var-only values (no equivalent flag) are tokens and other secrets — never pass these as argv.

**Discovery**: `cmd config.path` lists which env vars were observed at startup (key + redacted value for tokens, raw value otherwise) so an agent can verify the runtime environment.

---

## Initial command catalogue (~60 commands)

Naming pass applied: one verb = one operation. Mode flags split into separate commands when behavior differs (`sync.incremental` vs `sync.full` instead of `sync.run --full`). Every list-returning command supports `--limit` + `--offset`.

Format: `name` — args — D=destructive · I=non-idempotent · P=dry-run-previewable

**app** (5): `app.quit`; `app.version` (returns `{version, build, gitSha}`); `app.check_updates` I; `app.copy_selection`; `app.set_readonly` `{on:bool}` D.

**sync** (5): `sync.incremental` D·I·P (last-fetched delta; preview shows pending fetch count); `sync.full` D·I·P (replace-all; preview shows wipe + fetch counts); `sync.fetch_active_view` I; `sync.refresh_local` (rebuild local cache from disk); `sync.tracker_status` (returns `{state, lastSuccess, lastError}`).

**view** (6): `view.list` `{limit?, offset?}`; `view.get` `{id}`; `view.activate` `{id}`; `view.current`; `view.scroll_to_row` `{row:int}`; `view.refresh_active`.

**tickets** (5): `tickets.list_active` `{limit?, offset?}`; `tickets.search_active` `{query, limit?, offset?}`; `tickets.get` `{id}`; `tickets.history` `{id, limit?}`; `tickets.exists` `{id}` (returns `{exists:bool}`).

**ticket** (6): `ticket.set_field` `{id, field, value}` D·P; `ticket.set_fields` `{id, fields:object}` D·P; `ticket.transition` `{id, toStatus}` D·P; `ticket.add_comment` `{id, body}` D·I; `ticket.add_worklog` `{id, seconds, started?, comment?}` D·I; `ticket.create` `{draft:object}` D·I·P.

**fields** (4): `fields.list_available` `{limit?, offset?}`; `fields.get` `{id}`; `fields.refresh_catalog` I; `fields.icon_for` `{field, value}`.

**users** (3): `users.search` `{query, limit?}`; `users.watchers` `{ticketId}`; `users.votes` `{ticketId}`.

**attach** (2): `attach.open` `{ticketId, attachmentId}` D; `attach.download_preview` `{ticketId, attachmentId}` D·I.

**offline** (3): `offline.list_pending` `{limit?, offset?}`; `offline.replay_now` D·I·P; `offline.prune_dead` D·P.

**config** (4): `config.get` `{key?}`; `config.set` `{key, value}` D·P; `config.reload`; `config.path` (returns `{userData, runtimeAssets, env:{observed:[...]}}` — observed `SMATCHET_*` env vars with tokens redacted).

**perf** (5): `perf.snapshot` (returns rows inline JSON); `perf.dump` `{outPath?}` (writes to disk, returns `{file, count}`); `perf.reset`; `perf.frame_count`; `perf.toggle_panel` `{open?:bool}`.

**scenario** (3): `scenario.list`; `scenario.run` `{name, frames?, outPath?}` D·I (use `--stream` for progress); `scenario.cancel`.

**debug** (4): `debug.log` `{level, message}` (writes via Logger); `debug.thread_dump` (returns thread states); `debug.lua_eval` `{code}` D; `debug.mcp_status`.

**meta** (5): `commands.list` `{category?, limit?, offset?}`; `commands.help` `{name}`; `commands.invoke` `{name, args}`; `commands.recents` `{limit?}`; `commands.search` `{query, limit?}`.

**Inclusion rule:** include if (a) mutates persistent state, (b) returns user-facing data, (c) changes UI state visibly, or (d) composes the above (scenarios). Exclude per-frame `Tick*`, private getters used only by other commands, pure compute helpers.

**Pagination convention:** every command returning a list accepts `--limit` (default 50, max 500) and `--offset` (default 0). Response always includes `{items: [...], total: N, limit, offset, hasMore: bool}` so agents can paginate without hardcoded limits. Streamable lists (`scenario.run --stream`) use NDJSON instead.

---

## Frontend wiring

### CLI — talks to running app via MCP HTTP, falls back to spawn

Per the user's insight: prefer attaching to a running instance. Architecture:

1. `SmatchetStandalone.exe cmd <name> [--arg=value]…` — discover MCP endpoint, in this precedence order:
   - `--mcp-host=<host> --mcp-port=<port>` CLI flags (highest precedence).
   - `SMATCHET_MCP_HOST` / `SMATCHET_MCP_PORT` env vars.
   - `ConfigManager::GetUserDataDirectory() + "instance.json"` for `{pid, port}` (default). Written by `McpPlugin` on startup, deleted on shutdown.
   - If endpoint reachable: POST `{"name": "...", "arguments": {...}}` to `/mcp/tools/call`, print response JSON to stdout, exit with status from `CommandResult.Ok`.
   - If endpoint unreachable AND `--spawn` flag: boot app in "ephemeral" mode (windowed-but-auto-quit), dispatch in-process, write JSON to stdout, quit.
   - If endpoint unreachable AND no `--spawn`: print structured `not-connected` error to stdout (with hint suggesting `--spawn`), exit code 6.

2. **Output flags** (apply to every `cmd` invocation):
   - `--pretty` — indent the JSON document (2 spaces). Default is compact single-line.
   - `--quiet` / `-q` — extract the primary value from `data` and print bare. For lists, NDJSON one-per-line of just the id. For scalars, the value alone, no quotes. Designed for `xargs` / `grep` pipelines. Errors still go to stderr in JSON form.
   - `--format=table` — render `data` as a human-friendly text table (only for interactive terminals; auto-falls-back to JSON when stdout is piped). Never the default.
   - `--tokens` — instead of running the command, run it with output suppressed and print `{"tokens_estimate": N, "bytes": M}` to stderr. Lets agents pre-check before spending context on a large response. Estimator is `chars/4` for ASCII-heavy JSON — treat as **±30%**; not a substitute for a real tokenizer.
   - `--stream` — for long-running commands (scenarios), emit NDJSON progress events to stdout as the command runs; final document is the last line.
   - `--dry-run` — for commands with `DryRunSupported=true`: validate args, compute the diff, and return `{ok:true, dryRun:true, data:{wouldDo:...}}` without mutating. Commands without dry-run support return exit 9 / `dry-run-unsupported`. Combining with `--yes` is a no-op — dry-run never mutates, regardless of confirmation state.
   - `--timeout=<ms>` — for `--spawn` mode and `--stream` commands: cap the wait for async completion. On expiry: exit 8 / `timeout`. Default from `SMATCHET_SPAWN_TIMEOUT_MS` (120 s); set to `0` to disable.

3. **Help & discovery flags**:
   - `cmd <name> --help` — prints `Command::BuildHelpText()` to stdout (full schema + examples + idempotency + destructive flag), exits 0. This is the agent's path of least resistance for "how do I call this?".
   - `cmd --help` (no name) — prints categorized command list grouped by `Category`. Each category collapsed to one line per command (`name` + `summary`).
   - `cmd commands.list --json` — full machine-readable catalog (the canonical agent-discovery entry point; `--help` is for humans).

4. **Destructive guard**: `--yes` flag sets `ctx.ConfirmedDestructive` (also injects `__confirm:true` over HTTP). Without it, a destructive command exits 5 and prints a `confirm-required` error envelope. Never prompts. **Safe two-phase pattern for agents:** call once with `--dry-run` to preview the diff, then re-invoke with `--yes` once the preview is acceptable.

5. **Type coercion**: arg values from string to `ParamSpec.Type` — `Int`/`Bool`/`Number` via `std::stoi/stod` (with structured `validation-error` on failure naming the param); `Json` parsed via `nlohmann::json::parse`; `String` raw; `Bool` accepts `true|false|1|0|yes|no`. `Enum`-typed string params validated against `ParamSpec.Enum`.

6. **Stdout vs stderr**: stdout is always exactly one JSON document (or NDJSON stream with `--stream`). All logs, progress, warnings, `--tokens` previews → stderr. The one documented exception: `cmd <name> --help` prints human-readable text to stdout (agents should use `cmd commands.help --name=<n>` for JSON help). **Logger routing:** `main.cpp` detects CLI mode (presence of `cmd` token in argv) and calls `Logger::EnableCliStderrSink()` before `RegisterBuiltinCommands` runs, redirecting `LOG_*` macros to stderr instead of the standard file/UI sinks. This preserves CLAUDE.md's "no `std::cerr`" rule — logs still flow through Logger, just to a different sink.

7. **Exit codes** (stable contract):
   - `0` — `ok: true`
   - `2` — `unknown-command`
   - `3` — `validation-error` or `missing-required-arg`
   - `4` — `handler-error` or `backend-error` or `not-found`
   - `5` — `confirm-required` (destructive without `--yes`)
   - `6` — `not-connected` (no running instance, no `--spawn`)
   - `7` — transport error to running instance (HTTP failed)
   - `8` — `timeout` (spawn-mode async wait exceeded `--timeout` / `SMATCHET_SPAWN_TIMEOUT_MS`)
   - `9` — `dry-run-unsupported` (`--dry-run` passed to a command that doesn't implement it)

8. **`--help` text precision**: `Command::BuildHelpText()` produces text agents can grep. Format:
   ```
   tickets.search_active — Search active-view tickets by case-insensitive substring.

   Returns: { "items": [ { "id", "summary", "status" }, ... ], "total", "limit", "offset", "hasMore" }
   Idempotent: yes
   Destructive: no
   Dry-run:    no (read-only)
   Async-safe: yes

   Required:
     --query=<string>     case-insensitive substring to match in summary/id
   Optional:
     --limit=<int>        max matches to return (default: 50, max: 500)
     --offset=<int>       pagination offset (default: 0)

   Examples:
     SmatchetStandalone.exe cmd tickets.search_active --query=auth
     SmatchetStandalone.exe cmd tickets.search_active --query=PROJ-1 --limit=5 --quiet
   ```

### Command Palette — Ctrl+Shift+P modal

Top of `SmatchetUI::Draw`:
```cpp
if (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
    g_CommandPalette.Open();
}
g_CommandPalette.Draw(*this, app);
```

Modal layout (centered, 600×420, `BeginPopupModal("##cmdpalette", &open, NoTitleBar | AlwaysAutoResize)`):
- `InputTextWithHint("##q", "Type a command...", buf, 256, EnterReturnsTrue | AutoSelectAll)` — auto-focused on `IsWindowAppearing()`.
- Scrolled list: `app.Commands().All()` filtered by `FuzzyMatch(buf, cmd.Name + " " + cmd.Summary)` with score-descending sort. Empty `buf` shows `Recents()` first.
- Up/Down moves `selected_`, Enter dispatches, Esc closes.
- Required-args path: if selected command has any `Required` ParamSpec without a value, swap modal body to a per-param input form (one row per spec, type-aware widgets — checkbox for Bool, InputInt for Int, InputText for String/Json), then "Run" button.
- Destructive: row drawn red with `Colors::PriorityHigh`; "Run (Hold Shift)" gates dispatch on `KeyShift` to prevent accidental Enter.
- Result rendered inline below the list in a scrollable child window; toast for async-safe commands that complete after dispatch.
- Recents persisted to `<userData>/cmd_recents.json` (16-entry deque).

Reuses: `SmatchetTheme::ApplyStyle` palette, `ToLowerAsciiCopy`/`TrimCopy` from `StringUtil.h`, modal pattern from `BlameAnalysisUi.cpp:2364-2368`.

### MCP — registry merges into existing tool list

In `McpPlugin.cpp` `/mcp/tools/list` handler:
```cpp
for (const auto* c : app->Commands().All()) {
    j.push_back({{"name", c->Name},
                 {"description", c->Summary},
                 {"inputSchema", c->BuildJsonSchema()}});
}
// existing run_lua + Lua-registered tools follow
```

`/mcp/tools/call`: registry check first.
```cpp
if (app->Commands().Has(name)) {
    CommandContext ctx;
    ctx.App                  = app;
    ctx.Source               = CommandSource::Mcp;
    ctx.ConfirmedDestructive = arguments.value("__confirm",  false);
    ctx.DryRun               = arguments.value("__dry_run",  false);
    ctx.TimeoutMs            = arguments.value("__timeout_ms", 0);
    auto r = app->Commands().Dispatch(name, arguments, ctx);
    if (!r.Ok && cmdPtr->Destructive && !ctx.ConfirmedDestructive && !ctx.DryRun) {
        return ConfirmRequiredEnvelope(name, cmdPtr->Summary);
    }
    return WrapResult(r);
}
// fall through to legacy run_lua / Lua-registered handlers
```

Old tool names aliased: register `tickets.list_active` and `tickets.search_active` in the registry; in MCP `/mcp/tools/call`, if `name` is `list_active_tickets` or `search_active_tickets`, route to the new name. Old MCP clients keep working without code changes.

### Lua — `commands.invoke` + `register_global_action` migration

Add to `AppController_LuaBindings.cpp`:
```lua
local result = commands.invoke("ticket.set_field", { id="X-1", field="status", value="Done" })
-- result is a Lua table with {ok, error, data}
```

`ui.register_global_action(name, fn)` becomes sugar (field assignment — C++14 has no designated initializers, and aggregate positional init breaks every time we add a field):
```cpp
Command c;
c.Name        = "lua." + name;
c.Category    = "lua";
c.Summary     = "(Lua) " + name;
c.Destructive = false;
c.Idempotent  = true;
c.AsyncSafe   = true;
c.Handler     = [fn](const nlohmann::json&, CommandContext&) {
    fn();
    return CommandResult::Success({});
};
reg.Register(std::move(c));
```

The existing helper window listing `ui.register_global_action` actions stays as a category filter on the palette ("show only `lua.*`").

---

## Scenario subsystem

```cpp
// IScenario.h
class IScenario {
public:
    virtual ~IScenario() = default;
    virtual std::string Name() const = 0;
    virtual void OnStart(AppController& app, const nlohmann::json& args, std::string& outErr) = 0;
    virtual void OnFrame(AppController& app, int frameIndex) = 0;
    virtual bool IsDone(int frameIndex) const = 0;
    virtual nlohmann::json OnFinish(AppController& app) = 0;
};

class ScenarioRunner {
public:
    using Factory = std::function<std::unique_ptr<IScenario>()>;
    void RegisterFactory(const std::string& name, Factory f);
    CommandResult Start(const std::string& name, const nlohmann::json& args, CommandContext& ctx);
    void Tick(AppController& app);   // called once per frame from SmatchetUI::Draw
    void Cancel();
    bool Active() const;
    std::vector<std::string> ListNames() const;
private:
    std::unique_ptr<IScenario> active_;
    int frame_ = 0;
    std::string outPath_;
    bool quitOnFinish_ = true;
    std::unordered_map<std::string, Factory> factories_;
};
```

`ScenarioRunner::Tick` is called from `SmatchetUI::Draw` once per frame. When `IsDone`, calls `OnFinish`, writes JSON to `outPath_`, optionally calls `app.RequestAppQuit()` for `--spawn` mode (or just stops for attach-mode).

**Spawn-mode async wait contract.** When `--spawn` dispatches a command whose handler returns immediately but the real work continues asynchronously (`scenario.run`, `sync.full`, `sync.incremental`, `offline.replay_now`), `main.cpp` must block on a completion condition before emitting the final JSON envelope and quitting. Mechanism: each async-completing command (`AsyncSafe=false`) writes its terminal `CommandResult` into a one-shot `std::promise<CommandResult>` stored on `AppController` (`PendingAsyncResult()`); the spawn-mode driver waits on the matching `std::future` with `wait_for(ctx.TimeoutMs)`. On timeout it emits `{ok:false, error:{code:"timeout", details:{commandStillRunning:true}}}`, exits 8, and the app continues to drain in background before being killed by `app.RequestAppQuit()`. Sync-completing commands (every read-only command + most local-only mutations) have `AsyncSafe=true` and bypass the promise entirely — their dispatch result is the final result. `commands.help` exposes `asyncCompletes: !AsyncSafe` so agents pick a sensible `--timeout`.

### `PriorityGridScrollScenario`

**New session field** (in `Source_Core/include/SmatchetUiSession.h`, alongside other scenario plumbing):
```cpp
int  scenarioScrollTarget = -1;   // -1 = inactive; >=0 = pixel Y forced by active scenario
bool scenarioScrollActive = false;
```
Owned by `SmatchetUiSession`, written by scenarios, read by `SmatchetActiveProjectGridUi.cpp:341+` inside the `BeginTable` block via `if (session.scenarioScrollActive) ImGui::SetScrollY(static_cast<float>(session.scenarioScrollTarget));`. No file-static globals (addresses CODE_REVIEW's complaint).

- `OnStart(app, args)`: parse `frames` (default 600), `outPath` (default `<userData>/perf/priority-grid-scroll-<ts>.json`), optional `viewId`. Activate the view via `app.GetUiState().ViewState.Activate(viewId)`. Set `app.GetUiState().scenarioScrollActive = true; scenarioScrollTarget = 0;`. Reset `UiPerfMonitor`. If `ctx.DryRun`, skip activation + reset, return `{wouldDo:{frames, viewId, outPath}}` instead.
- `OnFrame(app, i)`: bump `app.GetUiState().scenarioScrollTarget` by `pixelsPerFrame` (default 8).
- `IsDone(i)`: `i >= frames`.
- `OnFinish(app)`: snapshot `UiPerfMonitor::Instance().GetLastFrameRows()` + EMA snapshot, return `{file, frames, rows: [...]}`. Writes the same JSON to `outPath_`. Clears `scenarioScrollActive=false`. Returns the data inline so attach-mode CLI gets it back too.

Adding a new scenario = one new `.cpp` + one `RegisterFactory` line in `BuiltinCommands.cpp`.

---

## Composability examples (to include in `cmd --help` and `PERF_WORKFLOW.md`)

These prove the design works in real shell pipelines:

```bash
# Find tickets matching a query, then fetch full details for each (via xargs):
SmatchetStandalone.exe cmd tickets.search_active --query=auth --quiet \
  | xargs -I{} SmatchetStandalone.exe cmd tickets.get --id={}

# Run a perf scenario, then read just the dominant row:
SmatchetStandalone.exe cmd scenario.run --name=priority-grid-scroll --frames=600 --yes \
  | jq '.data.rows | sort_by(-.lastTotalMs) | .[0]'

# Pre-check output size before pulling it into context:
SmatchetStandalone.exe cmd tickets.list_active --limit=500 --tokens
# stderr: {"tokens_estimate": 12450, "bytes": 49800}

# Stream a long-running scenario as NDJSON so the agent sees progress:
SmatchetStandalone.exe cmd scenario.run --name=priority-grid-scroll --frames=2000 --stream --yes

# Discover what's available without loading docs:
SmatchetStandalone.exe cmd commands.list --json | jq '.data.items[] | select(.category=="perf") | .name'

# Get full schema for a single command (agent's typical second step):
SmatchetStandalone.exe cmd ticket.set_field --help

# Two-phase safe mutation: preview, then execute if the diff is acceptable:
SmatchetStandalone.exe cmd ticket.set_field --id=PROJ-1 --field=status --value=Done --dry-run \
  | jq -e '.data.wouldDo.to == "Done"' \
  && SmatchetStandalone.exe cmd ticket.set_field --id=PROJ-1 --field=status --value=Done --yes

# Run with bounded async wait — fail fast on hung scenarios:
SmatchetStandalone.exe --spawn cmd scenario.run --name=priority-grid-scroll --frames=600 --timeout=30000 --yes
```

## Critical files to modify

- `Source_Core/include/AppController.h` — add accessors for `Commands()`, `Scenarios()`, `TickActiveScenario()`.
- `Source_Core/src/AppController.cpp` — own registry + scenario runner; call `RegisterBuiltinCommands` in `Initialize()`.
- `Source_Core/src/AppController_LuaBindings.cpp` — add `commands.invoke`, migrate `ui.register_global_action`.
- `Source_Core/src/SmatchetUI.cpp` — Ctrl+Shift+P poll, palette draw, scenario tick.
- `Source_Core/src/SmatchetActiveProjectGridUi.cpp:341` — inside `BeginTable` block, honor `session.scenarioScrollActive` by calling `ImGui::SetScrollY(static_cast<float>(session.scenarioScrollTarget))`.
- `Source_Core/include/SmatchetUiSession.h` — add `int scenarioScrollTarget = -1;` and `bool scenarioScrollActive = false;` members (see Scenario subsystem § for data-flow).
- `Target_Standalone/main.cpp` — extract arg parser; route `cmd` subcommand to HTTP client or `--spawn` in-process path.
- `Plugins/Mcp/McpPlugin.cpp:550-741` — merge registry into `/tools/list` and `/tools/call`. Write `instance.json` on startup, delete on shutdown.

## Existing utilities to reuse

- `nlohmann/json` — already linked, used for all command args/results/schemas.
- `cpp-httplib` — already linked; CLI HTTP client uses `httplib::Client`.
- `StringUtil.h`: `ToLowerAsciiCopy`, `TrimCopy`, `ContainsCaseInsensitive`.
- `SmatchetTheme.h`: `ApplyStyle`, `Colors::*` for palette styling.
- `UiPerfMonitor::Instance().GetLastFrameRows()` for `perf.snapshot` / `perf.dump` / scenarios.
- `ConfigManager::GetUserDataDirectory()` for default output paths and `instance.json` location.
- Modal pattern from `BlameAnalysisUi.cpp:2364-2368`.
- Input+filter pattern from `TicketFieldEditor.cpp:1300-1346`.
- Existing `Views::Activate` (`Source_Core/src/Views.cpp:27`) for view-switch commands.

---

## Verification (end-to-end test plan)

Run after the mega-PR lands on the branch, before merge to develop:

1. **Build both targets clean**:
   - `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — no warnings.

2. **Registry smoke** (new `Tests/CommandRegistryTests.cpp` — gtest if present, else CTest standalone):
   - Register / duplicate-throw / unknown-dispatch / param coercion / defaults / destructive guard.

3. **Palette UI**:
   - Launch app; press Ctrl+Shift+P; modal opens centered.
   - Type "sync"; only `sync.*` commands shown; arrow-down then Enter dispatches `sync.run`; UI refreshes.
   - Press Ctrl+Shift+P; type "prune"; row red; Enter alone does nothing; Shift+Enter dispatches.
   - Esc closes; reopen; Recents floats `sync.run` to top.

4. **CLI attach mode** (with a Smatchet instance running):
   - `SmatchetStandalone.exe cmd commands.list` → single-line JSON `{ok, command, data:{items, total, limit, offset, hasMore}}`, exit 0, includes ~60 commands.
   - `SmatchetStandalone.exe cmd commands.list --pretty` → indented JSON, same content.
   - `SmatchetStandalone.exe cmd commands.list --quiet` → bare command names one-per-line.
   - `SmatchetStandalone.exe cmd tickets.search_active --query=foo --limit=5` → JSON `{ok, command, data:{items:[...], total, ...}}`, exit 0.
   - `SmatchetStandalone.exe cmd ticket.set_field --help` → text help with schema + 2 examples on stdout, exit 0.
   - `SmatchetStandalone.exe cmd ticket.set_fild --query=foo` → exit 2 with `{ok:false, error:{code:"unknown-command", suggestions:["ticket.set_field", ...]}}`.
   - `SmatchetStandalone.exe cmd ticket.set_field --id=X-1` → exit 3 with `{ok:false, error:{code:"missing-required-arg", details:{param:"field"}}}` — never prompts.
   - `SmatchetStandalone.exe cmd offline.prune_dead` → exit 5 with `{ok:false, error:{code:"confirm-required", hint:"Re-run with --yes."}}`; with `--yes` → exit 0.
   - `SmatchetStandalone.exe cmd ticket.set_field --id=X-1 --field=status --value=Done --dry-run` → exit 0, JSON `{ok:true, dryRun:true, data:{wouldDo:{ticket:"X-1", field:"status", from:"...", to:"Done"}}}`; real ticket state unchanged after the call.
   - `SmatchetStandalone.exe cmd tickets.list_active --dry-run` → exit 9 with `{ok:false, error:{code:"dry-run-unsupported"}}` (read-only command).
   - `SmatchetStandalone.exe cmd tickets.list_active --limit=200 --tokens` → exit 0, no stdout, stderr `{"tokens_estimate": N, "bytes": M}`.
   - With no app running: `cmd commands.list` → exit 6, JSON `{ok:false, error:{code:"not-connected", hint:"Start Smatchet or pass --spawn."}}`.
   - `SmatchetStandalone.exe cmd config.path` → JSON `data.env.observed` lists every `SMATCHET_*` env var seen at startup, with `SMATCHET_TRACKER_TOKEN` value redacted as `"***"`.

5. **CLI spawn mode** (no app running):
   - `SmatchetStandalone.exe --spawn cmd scenario.run --name=priority-grid-scroll --frames=300 --yes` → window opens, scrolls, single-document JSON to stdout **after the scenario has actually finished** (verified by checking `data.frames == 300` and the output file exists), file at `<userData>/perf/priority-grid-scroll-<ts>.json`, app exits 0.
   - `SmatchetStandalone.exe --spawn cmd scenario.run --name=priority-grid-scroll --frames=300 --stream --yes` → NDJSON stream to stdout (one event per N frames), final line is the result document.
   - `SmatchetStandalone.exe --spawn cmd scenario.run --name=priority-grid-scroll --frames=10000 --timeout=2000 --yes` → exit 8, JSON `{ok:false, error:{code:"timeout", details:{commandStillRunning:true}}}`, no partial result on stdout.
   - `SMATCHET_TRACKER_TOKEN=... SMATCHET_TRACKER_BASE_URL=... SmatchetStandalone.exe --spawn cmd tickets.list_active --limit=5` → exit 0, JSON with items; same call without env vars and no on-disk config → exit 4, JSON `{ok:false, error:{code:"backend-error", hint:"Set SMATCHET_TRACKER_TOKEN or configure via config.set."}}`.

5b. **Composability sanity** (Bash, with running app):
   - `cmd tickets.search_active --query=foo --quiet | xargs -I{} cmd tickets.get --id={} --quiet | wc -l` produces non-zero count.
   - `cmd commands.list --json | jq '.data.items | length'` returns ~60.
   - `cmd scenario.run --name=priority-grid-scroll --frames=600 --yes | jq '.data.rows[0].name'` extracts dominant marker.

6. **MCP**:
   - `curl http://localhost:<port>/mcp/tools/list` → response includes registered names alongside `run_lua`.
   - `curl -X POST http://localhost:<port>/mcp/tools/call -d '{"name":"tickets.search_active","arguments":{"query":"foo"}}'` → wrapped result.
   - Same with `name=offline.prune_dead` and no `__confirm` → confirm-required envelope; with `__confirm:true` → success.

7. **Backwards-compat**:
   - Existing `--db-path / --backend-type / --mcp-port / --mcp-allow-remote / --help` flags still work unchanged.
   - Existing MCP tool name `list_active_tickets` still callable; routes to `tickets.list_active`.
   - Existing `ui.register_global_action("foo", function() ... end)` still appears in the helper window AND now in palette under category `lua`.

8. **Perf workflow doc update**:
   - Edit `docs/guides/perf-workflow.md`: replace the "hand off to user" section with `SmatchetStandalone.exe cmd scenario.run --name=priority-grid-scroll --frames=600` + `Read('<userData>/perf/...json')`. Cleanup section reads `cmd commands.list --category=perf` to confirm `temp:` markers are gone (the `temp:` audit is unchanged).

## Risks and open follow-ups

- **`instance.json` race.** Two simultaneous app instances would clobber the file. Mitigation: include PID + port; CLI verifies PID is alive before connecting. Final hardening can move to a per-port lock file.
- **MCP confirm phase pattern.** Already required by the project's prompt-injection-defense rules. Implementation needs care to match how AI agent hosts (Claude, etc.) handle two-phase confirmation envelopes — refer to existing MCP tool patterns before finalizing the envelope shape.
- **Param widget polish in palette.** First pass is functional but plain (one row per param, no inline help). Future PR could add per-param tooltips, enum dropdowns, JSON validation feedback.
- **Cancel mid-scenario.** `scenario.cancel` should be wired to also work from the palette — bind Esc inside an active scenario to dispatch it.
- **Dry-run fidelity for tracker mutations.** First-cut `wouldDo` for `ticket.set_field` is a local diff (current value → proposed value) computed without hitting the tracker. If the tracker's validation rules reject the value the real call would have failed too — but the preview won't know. Acceptable for v1; later, `--dry-run` could optionally call the tracker's "validate" endpoint where one exists (Jira `PUT /rest/api/3/issue/{id}?validate=true` etc.).
- **Token estimator accuracy.** `chars/4` heuristic is ±30% for JSON. Good enough for "is this 500 tokens or 50000 tokens?" but agents shouldn't budget against the estimate; clarified in `--tokens` help.

---

_End of plan._
