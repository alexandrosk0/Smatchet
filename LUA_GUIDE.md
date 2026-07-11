# Smatchet Lua Scripting Guide

Smatchet features a built-in Lua 5.3 runtime powered by `sol2`. This allows you to automate workflows across Jira tickets and customize how specific ticket fields are rendered in the Smatchet UI using Dear ImGui.

Scripts are executed in a secure, sandboxed environment. File I/O and OS-level operations are disabled to prevent malicious code execution, and strict instruction limits are enforced to prevent infinite loops from freezing the UI.

---

## 1. Directory Structure

Lua scripts are loaded from the `Scripts/` directory relative to the Smatchet executable (or current working directory). The two primary entry points are:

- **`Automation.lua`**: Evaluated when running bulk ticket automation.
- **`SmatchetHooks.lua`**: Edit it in **Automation -> Scripts & Actions...** (pick `SmatchetHooks.lua`, then **Save and Reload Hooks**). Not loaded automatically at process startup. Use it to register custom grid render hooks, ticket context-menu actions, global quick actions (see **Tools & Actions** tab), custom ImGui windows, MCP tools, and related UI hooks.
- **`RunLua.lua`**: Scratch script for **Save and Run** (same Lua globals as hooks); lives next to the other `*.lua` files under `Scripts/`.

---

## 2. API Reference

### Global Functions

| Function | Description |
| :--- | :--- |
| `log_info(msg)` | Logs a message to the internal Lua console. |
| `decode_json(json_str)` | Parses a JSON string. Returns two values: `(table_or_value, error_message)`. If successful, `error_message` is empty. |
| `register_field_display_cached(field_id, fn)` | Registers a cached cell renderer for a Jira `field_id`. The provider runs only on cache miss (when raw / avail_width / read_only / provider-gen changes); a recorded draw list is replayed on every other frame. See **Recorder API** below. |
| `unregister_field_display_cached(field_id)` | Removes the cached renderer for `field_id`. |
| `register_field_display_cached_by_name(name, fn)` | Same as `register_field_display_cached`, keyed on case-insensitive field display name (e.g. `"Progress"`). |
| `unregister_field_display_cached_by_name(name)` | Removes the cached renderer by display name. |

### Cached cell renderer — provider signature

```lua
register_field_display_cached("priority", function(issue_id, field_id, raw, avail_width, read_only, field_name, draw)
    -- record the cell into `draw`; the recording is cached and replayed each frame
    draw:image("Scripts/art/priority/high.png", 18, 18)
    return true   -- truthy → "I handled it"; false / nil → fall through to C++
end)
```

The 7th argument `draw` is a recorder. **You may not call `imgui.*` directly inside the provider** — Smatchet's `LuaImmediateModeGuard` raises `imgui.* not allowed inside cached provider / window — use draw:* instead` if you do. The recorder is required so C++ can replay the same operations between provider calls.

### `draw` recorder API

| Lua | Replays as |
| :--- | :--- |
| `draw:text(s)` | `ImGui::Text("%s", s)` |
| `draw:text_unformatted(s)` | `ImGui::TextUnformatted(s)` |
| `draw:image(path_or_url, w, h)` | `SmatchetFieldIconRender::DrawImagePathOrUrl` (texture cache + `ImGui::Image`) |
| `draw:progress_bar(frac, w, h, overlay?)` | `ImGui::ProgressBar` |
| `draw:same_line(offset?, spacing?)` | `ImGui::SameLine` |
| `draw:separator()` | `ImGui::Separator` |
| `draw:dummy(w, h)` | `ImGui::Dummy({w,h})` |
| `draw:push_color(col_idx, r, g, b, a)` | `ImGui::PushStyleColor` |
| `draw:pop_color(count?)` | `ImGui::PopStyleColor` |
| `draw:set_tooltip(s)` | `if hovered ImGui::SetTooltip("%s", s)` |
| `draw:button(label, fn(ticket_id, field_id))` | `ImGui::Button(label##idx)` — `fn` fires on click |
| `draw:input_text(label, initial, max_len, fn(ticket_id, field_id, new_value))` | `ImGui::InputText` — `fn` fires on commit (`IsItemDeactivatedAfterEdit`) |
| `draw:on_deactivated(fn)` | Attach to last interactive op; fires on `IsItemDeactivated` |
| `draw:on_deactivated_after_edit(fn)` | Attach to last interactive op; fires on `IsItemDeactivatedAfterEdit` |

The recorder auto-suffixes interactive widget labels with `##op<index>` so duplicate labels still get unique ImGui IDs.

**Callback fire-order**: `draw:on_deactivated` / `draw:on_deactivated_after_edit` are independent attachments — they do **not** replace the primary callback on `button(...)` / `input_text(...)`. On a click that also loses focus the same frame, both the button's click callback and the attached `on_deactivated*` callback fire. Same for `input_text`: the `on_commit` arg and an attached `:on_deactivated_after_edit(...)` both fire on `IsItemDeactivatedAfterEdit`. Author intent: opt-in additional hooks; if you don't want the double-fire, only use one of the two.

`draw:set_tooltip(s)` must follow an interactive op (`button` / `input_text`) — ImGui's `IsItemHovered()` checks the previously emitted widget; chaining `set_tooltip` after a non-interactive op (`text`, `separator`, `dummy`) is undefined and may render against an unrelated item.

If your provider reads Lua-global state outside the 6 documented args (theme tables, user prefs, action-mutated state), the strict cache-key comparison can't detect the change. Call one of these from the mutating callback:

- `ui.invalidate_field_cache()` — drop every cached cell.
- `ui.invalidate_field_cache_for(ticket_id)` — drop one ticket's cells.
- `ui.invalidate_field_cache_for(ticket_id, field_id)` — drop one cell.

### `imgui` Module

Available **only inside event-time callbacks** (`register_ticket_action`, `register_global_action`, MCP tools). Direct calls from inside a cached provider or a cached window draw fn raise `luaL_error` — use the `draw:*` recorder methods instead.

| Function | Description |
| :--- | :--- |
| `imgui.text(str)` | Renders text. |
| `imgui.text_unformatted(str)` | Renders text exactly as-is. |
| `imgui.progress_bar(fraction, w, h)` | Renders a progress bar. |
| `imgui.get_content_region_avail()` | Returns `(width, height)`. |
| `imgui.button("label")` | Returns `true` on click. |
| `imgui.same_line()` / `imgui.separator()` / `imgui.image(path, w, h)` | Layout helpers. |

### `ui` Module

You can construct completely custom dialogs and floating windows directly from Lua. Window draw functions follow the same recorder shape as cached cell renderers.

| Function | Description |
| :--- | :--- |
| `ui.register_window(name, fn)` | Registers a custom floating ImGui window titled `name`. `fn(draw)` is called only when the window is dirty (first paint, after `ui.invalidate_window`, after a `Click` / `Commit` callback fires, or after `NotifyLuaTicketDataChanged` / provider-gen bumps). The recording is replayed each frame between record events. |
| `ui.unregister_window(name)` | Removes the window. |
| `ui.invalidate_window(name)` | Forces the window to re-record on its next paint. Safe from any thread. |
| `ui.invalidate_field_cache()` / `ui.invalidate_field_cache_for(ticket_id [, field_id])` | Drops cached cell entries — see above. |
| `ui.register_ticket_action(name, callback_func_name)` | Row context-menu action. `name` is the menu label; registering again with the same `name` replaces the callback. `callback_func_name` is a top-level global function name; runs on a background thread. `my_callback(ticket)` receives the row's `Ticket`. See §3. |
| `ui.register_global_action(name, callback_func_name)` | Quick action on the **Tools & Actions** tab. Same replace-by-`name` semantics. See §3. |

### `mcp` Module

Smatchet acts as a Model Context Protocol (MCP) server. You can use Lua to define custom tools that connected AI agents can discover and execute.

| Function | Description |
| :--- | :--- |
| `mcp.register_tool(schema_table, fn)` | Registers an MCP Tool. `schema_table` must contain `name`, `description`, and either `parameters` (as a Lua table) or `parameters_json` (as a JSON string). `fn` receives the arguments as a Lua table and must return a string (or a table which will be JSON-encoded). |
| Built-in MCP `run_lua` tool | Optional core MCP tool (enabled in Preferences -> Integrations) that can execute a snippet or a script file under `Scripts/` with optional `args`. Uses the same sandbox and instruction-limit guard as other one-shot Lua entry points. |

### `ai` Module

Drives the AI assistant side panel (`Ctrl+Shift+A`) from Lua. Context blocks accumulate on the controller and ship with the next prompt; the panel's own context-block toggles are independent of the Lua-attached blocks. The panel-level system prompt (layered from `agents.md`) applies to every Lua-issued prompt — no separate Lua surface for that.

| Function | Description |
| :--- | :--- |
| `ai.add_context(block)` | Appends an `AiContextBlock` to the controller's context list. `block` is a table with `kind = "active_ticket" \| "multi_selected_tickets" \| "visible_grid_rows" \| "active_view" \| "audit_trail"`, `name = "..."`, `body = "..."`. Unknown `kind` defaults to `"active_ticket"`. |
| `ai.clear_context()` | Drops every block on the controller's context list. The panel-side auto-context toggles are untouched. |
| `ai.prompt(prompt [, extra_blocks])` | Submits `prompt` to the active provider; the controller's accumulated context blocks ship with it. Optional `extra_blocks` is an array of `AiContextBlock` tables appended via `add_context` immediately before submit. Returns nothing; the panel renders the streaming reply. |

```lua
-- Ask the assistant to summarise the active ticket plus some extra context.
ai.clear_context()
ai.add_context({ kind = "active_ticket", name = "Ticket", body = "PROJ-42" })
ai.prompt("Summarise this ticket and propose two next steps.")
```

**Threading**: `ai.*` is reachable only from UI-thread paths (`ui.register_ticket_action`, `ui.register_global_action`, `imgui.button` callbacks) — the `ai` global is bound only in the UI-thread Lua state, so referencing it from a background `process_ticket` worker or an automation script raises "attempt to index a nil value" immediately; there is no runtime race to avoid, just an absent global (Issue #1678). **Cost**: `ai.prompt` is event-time, never per-frame; it spawns a worker and returns immediately.

### `Ticket` Object

The `Ticket` object is passed to your automation scripts:

- `ticket.id`: The Jira Issue Key (e.g., `PROJ-123`).
- `ticket:get_field("field_id")`: Returns the raw value (usually a JSON string for complex fields or a raw text string) associated with that field.
- `ticket:set_field("field_id", "value")`: Schedules an offline field edit for the Jira issue. Smatchet will sync this to the server in the background. Returns `(success, error_msg)`.
- `ticket:transition("StatusName")`: Transitions the ticket to a new status (e.g. "Done"). Returns `(success, error_msg)`.

### `smatchet` Module

Global Smatchet application commands.

| Function | Description |
| :--- | :--- |
| `smatchet.get_ticket(id)` | Fetches a `Ticket` object by its Jira key from the local cache. Returns `(ticket, error_msg)`. |
| `smatchet.get_active_tickets()` | Returns a Lua table (array) of all `Ticket` objects currently loaded in Smatchet's active project grid. |
| `smatchet.create_issue(spec)` | Creates one issue on the **active** tracker (Jira or Plane) or queues an offline create. See below. |

**`smatchet.create_issue(spec)`** — `spec` is a Lua table:

- **Field mapping** matches CSV/JSON import: top-level keys like `summary`, `description`, `priority`, Jira field ids, and display names are resolved against the loaded field catalog. You may nest a `fields = { ... }` table (same semantics as JSON import) **or** use flat keys at the top level.
- **Defaults** (when omitted): same seed as the grid **new-issue** row: **default issue type** from Settings, plus **project / issue type from the last cached row** when the grid has issues and the spec does not override them. Since PR 6, project is **per-operation** — there is no global project preference; pass `projectkey` / `project_key` explicitly in the spec, or rely on the active view's project. If the cache is empty and Settings have no default issue type, Jira requires an explicit `issuetype` (e.g. `issuetype = "Task"`) or `issue_type_id` / `issue_type_name`.
- **Reserved / meta keys**: `offline` or `queue_offline` (boolean): when truthy, the draft is written to the **offline create queue** (SQLite) instead of calling the live API. Other unknown meta keys are ignored for forward compatibility.

**Returns `(result, preflight_err)`:**

- `preflight_err ~= ""` only for **pre-flight** problems (e.g. cache missing for offline queue, internal error before a result table exists). When `preflight_err == ""`, `result` is always a table.
- **Blocking:** for live creates, the call waits on the background create pipeline (`std::future::get()`), so the UI thread blocks until the tracker responds (same idea as running a create to completion).
- **Live create** — `result.ok`, `result.issue_key`, `result.error` mirror the internal create outcome. On validation failure, `result.missing_field_ids` is an array of field ids. If attachments failed after a successful create, `result.attachment_failures` is an array of `{ path = "...", reason = "..." }`.
- **Offline queue** — on success: `result.ok == true`, `result.offline_queued_id` set, and `result.issue_key == "offline:<id>"` (synthetic key for scripts). On queue failure: `result.ok == false`, `result.error` set.

### `tracker` compatibility (global)

Legacy shape for examples that use `tracker.get_type()` / `tracker.create_issue(fields)` (e.g. migration snippet in `SmatchetHooks.lua`):

| Function | Description |
| :--- | :--- |
| `tracker.get_type()` | Returns the configured backend type string (e.g. `Jira`, `Plane`), trimmed. |
| `tracker.create_issue(fields)` | Same draft/create path as `smatchet.create_issue`. Returns **`(issue_key, err)`**: on success `issue_key ~= ""` and `err == ""`; on failure `issue_key == ""` and `err` explains the error (including pre-flight failures). |

### `commands` Module

Calls any command in the unified command registry — the same registry behind the CLI, the Command Palette, and the MCP tools. One registration surfaces a command on every frontend, so anything you can run as `Smatchet.exe cmd <name>` is reachable here.

| Function | Description |
| :--- | :--- |
| `commands.invoke(name [, args])` | Dispatches the registered command `name`. `args` is an optional table of param → value. Returns a table `{ok = bool, data = <table>, error = <table>}` (the structured result envelope). |

This includes the `view.*` (saved-view CRUD) and `pane.*` (grid-pane scripting) groups. The pane group: `pane.list`, `pane.focus{id=...}`, `pane.next` / `pane.prev`, `pane.new` / `pane.duplicate` / `pane.split{direction=...}` (direction advisory), `pane.close{id=...}` (focused pane when omitted), and `pane.rename{title=..., id=...}` (session-scoped label override).

**Destructive commands require explicit confirmation.** Lua is a non-UI automation source: a command flagged destructive (e.g. `view.delete`, `sync.full`, `app.quit`) is **blocked** with `error.code = "confirm-required"` unless the args table carries `__confirm = true`. This must be a deliberate per-call flag — the binding never auto-confirms on your behalf (security audit 2026-06-13 #2/#3). Use `__dry_run = true` to preview without mutating.

```lua
-- Destructive: must pass __confirm explicitly, else confirm-required.
local r = commands.invoke("view.delete", { id = "abc", __confirm = true })
```

```lua
-- Focus the next grid pane, then rename it.
commands.invoke("pane.next")
local r = commands.invoke("pane.rename", { title = "Blockers" })
if not r.ok then
    print("rename failed: " .. (r.error and r.error.message or "?"))
end

-- Enumerate open panes.
local list = commands.invoke("pane.list")
for _, p in ipairs(list.data.items) do
    print(p.id, p.title, p.focused)
end
```

---

## 3. Writing Scripts

### Automation Scripts (`Automation.lua`)

You can edit `Automation.lua` inside Smatchet from **Automation -> Scripts & Actions...** (file picker -> `Automation.lua`) using the built-in code editor.
When you click **Save and Run on Selection**, Smatchet compiles the **currently selected** `.lua` file and invokes `process_ticket(ticket)` **only for the tickets you have currently selected** in the active project grid (e.g., via Shift+Click or Ctrl+Click). Only scripts that define `process_ticket` are useful here (by convention `Automation.lua`).

You must define the `process_ticket` function in your script:

```lua
-- Automation.lua
function process_ticket(ticket)
    -- Read a custom field value
    local raw_val = ticket:get_field("customfield_10020")
    
    if raw_val then
        log_info("Ticket " .. ticket.id .. " has field value: " .. raw_val)
    end
end
```

### UI hooks setup (`SmatchetHooks.lua`)

If you want to change how a specific field is rendered in the spreadsheet (e.g., drawing a progress bar instead of showing raw JSON text), you can register a handler in `SmatchetHooks.lua`.

The function you register will receive the following positional arguments:
1. `issue_id` (string): The issue key (e.g., `PROJ-123`).
2. `field_id` (string): The internal Jira field ID.
3. `raw_value` (string): The raw field data.
4. `avail_width` (number): The pixel width available in the column.
5. `read_only` (boolean): Whether the user is allowed to edit this field.
6. `field_name` (string | nil): The human-readable name of the field.

**Return Value:** Your function **must** return `true` if it successfully rendered the UI. If it returns `false` or `nil`, Smatchet will fall back to its default text rendering.

```lua
-- SmatchetHooks.lua

-- Define our cached render handler. The 7th arg `draw` records ops; Smatchet replays the
-- recording each frame until raw / avail_width / read_only changes — Lua runs at most once
-- per cell per refresh.
local function render_progress_bar(issue_id, field_id, raw, avail_width, read_only, field_name, draw)
    -- Attempt to parse the raw Jira JSON data
    local data, err = decode_json(raw)

    if not data or err ~= "" then
        return false -- Let Smatchet render the fallback error text
    end

    local current = tonumber(data.progress) or 0
    local total = tonumber(data.total) or 0
    local fraction = 0

    if total > 0 then
        fraction = current / total
    end

    -- Record a progress-bar op. Replayed in C++ on every subsequent frame.
    draw:progress_bar(fraction, avail_width, 0)

    return true -- Tell Smatchet we handled the drawing
end

-- Register the handler to trigger for any field named "Progress"
register_field_display_cached_by_name("Progress", render_progress_bar)
```

### Context menu and menu bar actions (`SmatchetHooks.lua`)

Register these from the same setup script you use for field display (typically `SmatchetHooks.lua`). After editing, pick **SmatchetHooks.lua** in the Scripts & Actions window and use **Save and Reload Hooks** so registrations apply.

**Ticket actions** — Right-click **any cell** on a ticket row: **Copy** and **Lua Actions** apply to that issue. **Quick comment templates** still appear only when you right-click the **issue key** column. **Shift+right-click** opens the raw-value popup; Lua ticket actions are listed there as well.

**Global actions** — Open **Automation -> Scripts & Actions...** and use the **Tools & Actions** tab (or run the same callbacks from your own Lua). Runs with no selected row; use `smatchet.get_active_tickets()` for bulk work. Reopen the window from **Automation -> Scripts & Actions...** if you closed it.

Callbacks share the same instruction budget as other one-shot Lua work (see section 4). There is no dedicated `ai` Lua module for prompts or context; use `log_info`, MCP tools, or automation flows separately.

**Example — ticket row (log description snippet):**

```lua
function preview_description(ticket)
    local desc = ticket:get_field("description") or ""
    if #desc > 200 then
        desc = desc:sub(1, 200) .. "..."
    end
    log_info(ticket.id .. " description: " .. desc)
end

ui.register_ticket_action("Log description preview", "preview_description")
```

**Example — global bulk field edit:**

`assignee` values must be valid for Jira (often a JSON user payload). There is no `smatchet.get_current_user()` helper yet; substitute your account id or fetch it outside Lua.

```lua
function bulk_set_priority()
    local tickets = smatchet.get_active_tickets()
    for _, t in ipairs(tickets) do
        local ok, err = t:set_field("priority", "High")
        if not ok then
            log_info(t.id .. " failed: " .. err)
        end
    end
end

ui.register_global_action("Bulk set priority (example)", "bulk_set_priority")
```

### Dynamic MCP Tools (For AI Agents)

You can define custom tools that any AI agent connected to Smatchet's MCP server can use. This allows you to teach the AI how to do studio-specific workflows.

```lua
-- Register a custom tool for the LLM
mcp.register_tool({
    name = "assign_ticket",
    description = "Assigns a ticket to a specific user",
    parameters_json = [[
        {
            "type": "object",
            "properties": {
                "ticket_id": { "type": "string" },
                "user": { "type": "string" }
            },
            "required": ["ticket_id", "user"]
        }
    ]]
}, function(params_json)
    -- Parse the arguments provided by the LLM
    local params = decode_json(params_json)
    
    local ticket_id = params.ticket_id
    local user = params.user
    
    local ticket, err = smatchet.get_ticket(ticket_id)
    if not ticket then
        return '{"status": "error", "message": "Ticket not found: ' .. err .. '"}'
    end
    
    log_info("AI is assigning " .. ticket_id .. " to " .. user)
    
    local ok, edit_err = ticket:set_field("assignee", '{"accountId": "' .. user .. '"}')
    if not ok then
        return '{"status": "error", "message": "Failed to set field: ' .. edit_err .. '"}'
    end
    
    return '{"status": "success", "message": "Ticket assigned successfully."}'
end)
```

### Custom ImGui Windows

You can create standalone floating windows to build internal tools or visualize data.

```lua
-- SmatchetHooks.lua
local click_count = 0

ui.register_window("My Custom Tool", function()
    imgui.text("Welcome to my studio-specific tool.")
    imgui.separator()
    
    if imgui.button("Click Me!") then
        click_count = click_count + 1
        log_info("Button clicked " .. tostring(click_count) .. " times!")
    end
    
    imgui.text("Count: " .. tostring(click_count))
    
    if click_count > 5 then
        imgui.text("You are clicking a lot!")
    end
end)
```


---

## 4. Environment & Limitations

To ensure application stability, Smatchet enforces the following rules on all Lua executions:

1. **Sandboxing**: The standard `os`, `io`, `package`, `require`, `dofile`, and `load` libraries are removed from the execution environment. You cannot read/write files or execute arbitrary shell commands.
2. **Instruction Limit**: Smatchet uses Lua debug hooks to prevent runaway scripts.
    - Initial setup scripts (`SmatchetHooks.lua`) are limited to **100,000 instructions**.
    - Ticket context-menu actions, global quick actions (**Scripts & Actions** -> **Tools & Actions**), MCP tool callbacks, and automation window Lua windows use **100,000 instructions** per invocation (same hook as setup-style runs).
    - Ticket processing (`process_ticket`) and UI rendering handlers are limited to **10,000 instructions**.
    If a script exceeds this limit (e.g., via `while true do end`), it will be immediately aborted and an error will be printed to the Lua Console.
