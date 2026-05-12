# Smatchet CLI Guide

The Smatchet CLI exposes the full [unified Command System](backlog/COMMAND_SYSTEM_PLAN.md) from the shell. It connects to a running Smatchet instance over its MCP HTTP endpoint and dispatches any registered command — the same catalog that feeds the in-app Command Palette (Ctrl+Shift+P), MCP tools, and Lua automation.

## Contents

- [Quick start](#quick-start)
- [Connection and discovery](#connection-and-discovery)
- [Flags](#flags)
- [Exit codes](#exit-codes)
- [Output format](#output-format)
- [Command catalogue](#command-catalogue)
  - [commands](#commands) · [app](#app) · [sync](#sync) · [view](#view)
  - [tickets](#tickets) · [ticket](#ticket) · [fields](#fields) · [users](#users)
  - [attach](#attach) · [offline](#offline) · [config](#config)
  - [perf](#perf) · [scenario](#scenario) · [debug](#debug)
- [Config overrides](#config-overrides)
- [Environment variables](#environment-variables)
- [Composability examples](#composability-examples)
- [Perf workflow](#perf-workflow)

---

## Quick start

```bash
# Requires a running Smatchet instance with MCP enabled (mcp_enabled: true in config).

# List all available commands
SmatchetStandalone.exe cmd commands.list --pretty

# Get full schema for one command
SmatchetStandalone.exe cmd commands.help --name=tickets.search_active

# Search tickets
SmatchetStandalone.exe cmd tickets.search_active --query=auth --limit=5

# Pipe ticket IDs into a second command
SmatchetStandalone.exe cmd tickets.list_active --quiet | head -10

# Preview a config change without applying it
SmatchetStandalone.exe cmd config.set --key=logMinLevel --value=debug --dry-run

# Run a perf benchmark scenario
SmatchetStandalone.exe cmd perf.reset
SmatchetStandalone.exe cmd scenario.run --name=priority-grid-scroll --frames=600 --yes
SmatchetStandalone.exe cmd perf.snapshot --pretty
```

---

## Connection and discovery

The CLI attaches to a **running** Smatchet instance over its MCP HTTP endpoint. No separate server process is needed.

**Endpoint discovery** (highest priority first):

| Source | How |
|---|---|
| Explicit flags | `--mcp-host=<host> --mcp-port=<port>` |
| Environment | `SMATCHET_MCP_HOST` / `SMATCHET_MCP_PORT` |
| `instance.json` | `<userData>/instance.json` written by the app on startup; PID is verified alive before the port is trusted |
| Default | `127.0.0.1:42360` |

**Enable MCP in the app** by setting `"mcp_enabled": true` in `smatchet_config.json`, or via:

```bash
SmatchetStandalone.exe cmd config.set --key=mcpEnabled --value=true
```

If no instance is reachable, the CLI exits with code **6** (`not-connected`) and prints a structured error to stderr.

---

## Flags

All flags apply to every `cmd` invocation.

| Flag | Description |
|---|---|
| `--pretty` | Indent the stdout JSON (2 spaces). Default is compact single-line. |
| `--quiet` / `-q` | Bare scalar output for shell pipelines. Lists print one id/name per line; scalars print the value alone. Errors still go to stderr. |
| `--yes` | Confirm a destructive command. Without it the command exits **5** (`confirm-required`) and never prompts. |
| `--dry-run` | Preview a mutation: validate args, compute the `wouldDo` diff, return without mutating. Commands without dry-run support exit **9** (`dry-run-unsupported`). |
| `--tokens` | Estimate output size in tokens (`bytes ÷ 4`, ±30%) and print `{tokens_estimate, bytes}` to stderr. No stdout. Use to pre-check before pulling large results into agent context. |
| `--timeout=<ms>` | Cap async wait; passed as `__timeout_ms` to the server. Default: `SMATCHET_SPAWN_TIMEOUT_MS` or no cap. |
| `--mcp-host=<host>` | Override the MCP host (default: `127.0.0.1` / `SMATCHET_MCP_HOST`). |
| `--mcp-port=<int>` | Override the MCP port (default: `SMATCHET_MCP_PORT` / `instance.json` / `42360`). |
| `--help` / `-h` | After a command name: print human-readable schema. Without a name: print this flag summary. |

### Safe two-phase pattern for destructive commands

```bash
# 1. Preview what would change
SmatchetStandalone.exe cmd ticket.set_field --id=PROJ-1 --field=status --value=Done --dry-run

# 2. Execute only if the preview is acceptable
SmatchetStandalone.exe cmd ticket.set_field --id=PROJ-1 --field=status --value=Done --yes
```

---

## Exit codes

Stable across versions. Agents should branch on these.

| Code | Meaning |
|---|---|
| `0` | Success (`ok: true`) |
| `2` | Unknown command |
| `3` | Missing required arg or validation error |
| `4` | Handler error, backend error, or not found |
| `5` | Confirm required — destructive command without `--yes` |
| `6` | Not connected — no running instance reachable |
| `7` | Transport error — HTTP failed after connecting |
| `8` | Timeout — async wait exceeded `--timeout` |
| `9` | Dry-run unsupported — `--dry-run` passed to a read-only command |

---

## Output format

**Stdout** is always exactly one JSON document (canonical envelope):

```json
{ "ok": true,  "command": "tickets.list_active", "data": { "items": [...], "total": 38, "limit": 10, "offset": 0, "hasMore": true } }
{ "ok": false, "command": "tickets.serch_active", "error": { "code": "unknown-command", "message": "...", "suggestions": ["tickets.search_active"] } }
```

Dry-run responses add `"dryRun": true` and a `"wouldDo"` payload:

```json
{ "ok": true, "command": "ticket.set_field", "dryRun": true, "data": { "wouldDo": { "ticket": "X-1", "field": "status", "from": "In Progress", "to": "Done" } } }
```

**Stderr** is for errors, `--tokens` estimates, and diagnostics only. Stdout is always parseable JSON.

**Pagination**: list commands accept `--limit` (default 50, max 500) and `--offset` (default 0). Every list response includes `{items, total, limit, offset, hasMore}`.

---

## Command catalogue

### commands

Agent discovery entry points.

| Command | Params | Notes |
|---|---|---|
| `commands.list` | `category?`, `limit?`, `offset?` | Full catalog; filter by category name |
| `commands.help` | `name` *(required)* | Full schema + helpText + examples |
| `commands.search` | `query` *(required)*, `limit?` | Fuzzy match by name or summary |
| `commands.recents` | `limit?` | Most recently dispatched names |

```bash
SmatchetStandalone.exe cmd commands.list --category=perf --quiet
SmatchetStandalone.exe cmd commands.help --name=scenario.run --pretty
SmatchetStandalone.exe cmd commands.search --query=sync
```

---

### app

| Command | Params | Notes |
|---|---|---|
| `app.version` | — | `{version, releaseRepo}` |
| `app.quit` | — | Destructive (`--yes`). Requests graceful shutdown. |
| `app.check_updates` | — | Queries GitHub for a newer release |
| `app.set_readonly` | `on` *(bool, required)* | Destructive (`--yes`). Persists to config. Dry-run supported. |

---

### sync

| Command | Params | Notes |
|---|---|---|
| `sync.incremental` | — | Delta sync from last cursor. Async. Dry-run shows pending count. |
| `sync.full` | — | Destructive (`--yes`). Wipe + full re-fetch. Dry-run supported. |
| `sync.refresh_local` | — | Rebuild in-memory list from SQLite; no network. |
| `sync.fetch_active_view` | — | Fetch active view inline (no cache write). Returns tickets directly. |
| `sync.tracker_status` | — | `{state, syncWarning, fieldCatalogError}` |

---

### view

Registered once `ViewState` is loaded (first render frame). Returns `{id, name, jql, fields}` objects.

| Command | Params | Notes |
|---|---|---|
| `view.list` | `limit?`, `offset?` | All configured views |
| `view.get` | `id` *(required)* | Single view by id |
| `view.current` | — | Currently active view |
| `view.activate` | `id` *(required)* | Switch active view + trigger sync |
| `view.refresh_active` | — | Re-sync active view from tracker |

```bash
SmatchetStandalone.exe cmd view.list --quiet          # print view ids
SmatchetStandalone.exe cmd view.activate --id=mine   # switch and sync
```

---

### tickets

Read-only queries against the active view's loaded ticket cache.

| Command | Params | Notes |
|---|---|---|
| `tickets.list_active` | `limit?`, `offset?` | Returns `{id, summary?, status?}` per item. Alias: `list_active_tickets` |
| `tickets.search_active` | `query` *(required)*, `limit?`, `offset?` | Case-insensitive substring across id + all field values. Alias: `search_active_tickets` |
| `tickets.get` | `id` *(required)* | Full field map `{id, fields:{fieldId: value, ...}}` |
| `tickets.exists` | `id` *(required)* | `{exists: bool, id}` |

---

### ticket

Mutations — all destructive (`--yes`), dry-run supported where noted.

| Command | Params | Notes |
|---|---|---|
| `ticket.set_field` | `id`, `field`, `value` *(all required)* | Update one field. Dry-run returns `{wouldDo:{ticket, field, from, to}}`. |
| `ticket.set_fields` | `id` *(required)*, `fields` *(JSON object)* | Update multiple fields in one call. Dry-run supported. |
| `ticket.transition` | `id`, `toStatus` *(both required)* | Change status. Dry-run supported. |
| `ticket.add_comment` | `id`, `body` *(both required)* | Post plain-text comment. |
| `ticket.add_worklog` | `id`, `seconds` *(both required)*, `started?`, `comment?` | Log time. `seconds` is converted to `1h 30m` notation. |
| `ticket.create` | `projectKey`, `summary` *(both required)*, `issueType?`, `offline?` | Create live or queue offline. Dry-run supported. |

```bash
# Two-phase safe field edit
SmatchetStandalone.exe cmd ticket.set_field --id=PROJ-1 --field=priority --value=High --dry-run
SmatchetStandalone.exe cmd ticket.set_field --id=PROJ-1 --field=priority --value=High --yes

# Bulk field update
SmatchetStandalone.exe cmd ticket.set_fields --id=PROJ-1 --fields='{"priority":"High","labels":"needs-review"}' --yes

# Queue a create for later
SmatchetStandalone.exe cmd ticket.create --projectKey=PROJ --summary="Fix auth bug" --offline=true --yes
```

---

### fields

| Command | Params | Notes |
|---|---|---|
| `fields.list_available` | `limit?`, `offset?` | Returns `{id, name, type}` per field |
| `fields.get` | `id` *(required)* | Single field metadata |
| `fields.refresh_catalog` | — | Re-fetch from backend |
| `fields.icon_for` | `field`, `value` *(both required)* | Resolve Lua icon map path/URL for a field+value |

---

### users

| Command | Params | Notes |
|---|---|---|
| `users.search` | `query` *(required)*, `limit?` | Search by display-name substring. Returns `{id, displayName, email}`. |
| `users.watchers` | `ticketId` *(required)* | List watchers for a ticket |
| `users.votes` | `ticketId` *(required)* | `{voteCount, voters:[{id, displayName}]}` |

---

### attach

| Command | Params | Notes |
|---|---|---|
| `attach.open` | `url`, `filename` *(both required)*, `mimeType?` | Destructive (`--yes`). Downloads + opens in viewer. |
| `attach.download_preview` | `url`, `filename` *(both required)*, `mimeType?` | Download to temp file for in-app preview. |

---

### offline

| Command | Params | Notes |
|---|---|---|
| `offline.list_pending` | `limit?`, `offset?` | Lists queued creates + field edits separately |
| `offline.replay_now` | — | Destructive (`--yes`). Replay all queued items now. Dry-run shows counts. |
| `offline.prune_dead` | — | Destructive (`--yes`). Delete all dead-letter items. Dry-run shows counts. |

---

### config

| Command | Params | Notes |
|---|---|---|
| `config.get` | `key?` | One key or all safe keys. Never exposes credentials. |
| `config.set` | `key`, `value` *(both required)* | Persist to `smatchet_config.json`. See [Config overrides](#config-overrides). |
| `config.reload` | — | Invalidate cache; next `Load()` re-reads from disk. |
| `config.path` | — | `{userData, runtimeAssets, imGuiSettings, env:{observed:[...]}}` |

---

### perf

| Command | Params | Notes |
|---|---|---|
| `perf.snapshot` | — | Per-scope rows from the last drawn frame |
| `perf.dump` | `outPath?` | Write snapshot to JSON; returns `{file, count}` |
| `perf.reset` | — | Clear all accumulated measurements before a benchmark run |
| `perf.frame_count` | — | `{scopeCount, totalCalls}` from the last frame |
| `perf.toggle_panel` | `open?` *(bool)* | Show/hide the Performance Monitor panel. Omit to toggle. Dry-run supported. |

---

### scenario

Automated multi-frame test scenarios driven inside the render loop.

| Command | Params | Notes |
|---|---|---|
| `scenario.list` | — | Names of registered scenarios |
| `scenario.run` | `name` *(required)*, `frames?` (default 600), `outPath?` | Destructive (`--yes`). Starts scenario; result JSON written to `outPath`. |
| `scenario.cancel` | — | Abort the active scenario |

Built-in scenario: **`priority-grid-scroll`** — drives the priority grid at 8 px/frame for N frames, collecting per-scope perf timings.

```bash
SmatchetStandalone.exe cmd scenario.list
SmatchetStandalone.exe cmd scenario.run --name=priority-grid-scroll --frames=300 --yes
```

---

### debug

| Command | Params | Notes |
|---|---|---|
| `debug.log` | `message` *(required)*, `level?` (trace/debug/info/warn/error) | Emit a Logger entry from the agent. Useful as breadcrumbs. |
| `debug.lua_eval` | `code` *(required)* | Destructive (`--yes`). Evaluate a Lua snippet; returns `{result}`. |
| `debug.mcp_status` | — | `{mcpEnabled, hasClientActivity, lastActivityMsAgo, activityLog:[...]}` |
| `debug.thread_dump` | — | `{hardwareConcurrency}` |

---

## Config overrides

`config.set` writes to `smatchet_config.json` with immediate cache invalidation. Changes take effect on the next `ConfigManager::Load()` call inside the running app (next sync, next command invocation, etc.).

### Allowlisted keys

Keys that require a plugin or app restart to take effect are noted.

| Key | Type | Default | Notes |
|---|---|---|---|
| `logMinLevel` | string | `info` | `trace`/`debug`/`info`/`warn`/`error` |
| `logTrackerHttpBodies` | bool | `false` | Log truncated HTTP bodies at Trace |
| `readOnlyMode` | bool | `false` | Disable all tracker mutations |
| `enableFieldOverflowTooltips` | bool | `true` | Show overflow tooltips on hover |
| `showPerformance` | bool | `false` | Show Performance Monitor panel |
| `showLogWindow` | bool | `false` | Show Runtime Log panel |
| `jqlQuery` | string | `assignee=currentUser()` | JQL for next sync |
| `domain` | string | — | Tracker domain/URL *(reconnect required)* |
| `email` | string | — | Tracker email *(reconnect required)* |
| `projectKey` | string | — | Default project for new issues |
| `trackerType` | string | `Jira` | `Jira` or `Plane` *(restart required)* |
| `planeUrl` | string | — | Plane API origin *(reconnect required)* |
| `planeWorkspaceSlug` | string | — | Plane workspace slug *(reconnect required)* |
| `planeProjectId` | string | — | Plane project UUID *(reconnect required)* |
| `mcpEnabled` | bool | `false` | Start MCP plugin *(plugin restart required)* |
| `mcpPort` | int | `42360` | MCP listen port *(plugin restart required)* |
| `mcpAllowRemote` | bool | `false` | Bind all interfaces *(plugin restart required)* |
| `mcpAllowLuaExecution` | bool | `false` | Allow `run_lua` in MCP *(plugin restart required)* |

**Credentials are not in this table** — pass them via environment variables (see below).

```bash
# Change the JQL filter, then trigger a sync
SmatchetStandalone.exe cmd config.set --key=jqlQuery --value="project=BLOOP AND status != Done"
SmatchetStandalone.exe cmd sync.incremental --yes

# Preview a change without applying
SmatchetStandalone.exe cmd config.set --key=mcpPort --value=8765 --dry-run
```

---

## Environment variables

Applied by the app at startup. All are stable API — renaming is a breaking change.

**Precedence**: explicit CLI flag > env var > config file > built-in default.

| Variable | Maps to | Notes |
|---|---|---|
| `SMATCHET_TRACKER_TOKEN` | `cfg.ApiToken` (Jira) / `cfg.PlaneApiKey` (Plane) | Never pass as argv — always use env |
| `SMATCHET_TRACKER_BASE_URL` | `cfg.Domain` (Jira) / `cfg.PlaneUrl` (Plane) | Tracker origin URL |
| `SMATCHET_LOG_LEVEL` | `cfg.LogMinLevel` | `trace`/`debug`/`info`/`warn`/`error` |
| `SMATCHET_USER_DATA` | `ConfigManager::GetUserDataDirectory()` | Applied before first `Load()`; redirects config, DB, views, recents |
| `SMATCHET_MCP_PORT` | `cfg.McpPort` | Override MCP listen port |
| `SMATCHET_MCP_ALLOW_REMOTE` | `cfg.McpAllowRemote` | `true`/`1` to bind all interfaces |
| `SMATCHET_DB_PATH` | `cfg.DbPath` | Override SQLite database path |
| `SMATCHET_BACKEND_TYPE` / `SMATCHET_TRACKER_TYPE` | `cfg.TrackerType` | `Jira` or `Plane` |
| `SMATCHET_MCP_HOST` | CLI endpoint discovery only | Override host for CLI → app connection |
| `SMATCHET_SPAWN_TIMEOUT_MS` | CLI `--timeout` default | `0` = no cap (default) |

`config.path` lists which `SMATCHET_*` vars were observed at startup (token values redacted as `***`):

```bash
SmatchetStandalone.exe cmd config.path --pretty
```

---

## Composability examples

```bash
# Search active tickets then fetch full detail for each
SmatchetStandalone.exe cmd tickets.search_active --query=auth --quiet \
  | xargs -I{} SmatchetStandalone.exe cmd tickets.get --id={}

# Pre-check output size before pulling into agent context
SmatchetStandalone.exe cmd tickets.list_active --limit=500 --tokens
# stderr: {"tokens_estimate":12450,"bytes":49800}

# Pipe command names into help
SmatchetStandalone.exe cmd commands.list --category=perf --quiet \
  | xargs -I{} SmatchetStandalone.exe cmd commands.help --name={}

# Machine-readable discovery — count all commands
SmatchetStandalone.exe cmd commands.list | jq '.data.total'

# Filter by category
SmatchetStandalone.exe cmd commands.list | jq '.data.items[] | select(.category=="tickets") | .name'

# Paginate through all tickets
SmatchetStandalone.exe cmd tickets.list_active --limit=50 --offset=0 --quiet
SmatchetStandalone.exe cmd tickets.list_active --limit=50 --offset=50 --quiet
```

---

## Perf workflow

Measure UI performance without a human driver.

```bash
# 1. Instrument hot paths (add SMATCHET_UI_PERF_SCOPE("temp:...") markers)
# 2. Build
cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone

# 3. With app running — reset, run scenario, read results
SmatchetStandalone.exe cmd perf.reset
SmatchetStandalone.exe cmd scenario.run --name=priority-grid-scroll --frames=600 --yes
SmatchetStandalone.exe cmd perf.snapshot --pretty

# Sort by dominant scope
SmatchetStandalone.exe cmd perf.snapshot | jq '.data.rows | sort_by(-.lastTotalMs) | .[0:5]'

# Write to file
SmatchetStandalone.exe cmd perf.dump --outPath=C:/tmp/perf-baseline.json

# Confirm temp: markers removed after fix
SmatchetStandalone.exe cmd perf.snapshot --quiet | grep temp:   # → no output expected
```

See [`.claude/PERF_WORKFLOW.md`](.claude/PERF_WORKFLOW.md) for the full profiling methodology.

---

## See also

- [MCP Guide](MCP_GUIDE.md) — using Smatchet as an MCP server for AI agents
- [Lua Scripting Guide](LUA_GUIDE.md) — `commands.invoke()` from Lua
- [Build Guide](BUILD.md) — building the standalone executable
