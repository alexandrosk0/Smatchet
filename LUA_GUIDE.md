# Smatchet Lua Scripting Guide

Smatchet features a built-in Lua 5.3 runtime powered by `sol2`. This allows you to automate workflows across Jira tickets and customize how specific ticket fields are rendered in the Smatchet UI using Dear ImGui.

Scripts are executed in a secure, sandboxed environment. File I/O and OS-level operations are disabled to prevent malicious code execution, and strict instruction limits are enforced to prevent infinite loops from freezing the UI.

---

## 1. Directory Structure

Lua scripts are loaded from the `Scripts/` directory relative to the Smatchet executable (or current working directory). The two primary entry points are:

- **`Automation.lua`**: Evaluated when running bulk ticket automation.
- **`SmatchetHooks.lua`**: Edit it in **Lua & Automation** → **SmatchetHooks.lua** tab (save to disk, then **Save && Reload hooks** to apply). Not loaded automatically at process startup. Use it to register custom grid render hooks, ticket context-menu actions, global **Scripts** actions, custom ImGui windows, MCP tools, and related UI hooks.

---

## 2. API Reference

### Global Functions

| Function | Description |
| :--- | :--- |
| `log_info(msg)` | Logs a message to the internal Lua console. |
| `decode_json(json_str)` | Parses a JSON string. Returns two values: `(table_or_value, error_message)`. If successful, `error_message` is empty. |
| `register_field_display(field_id, fn)` | Registers a custom render function for a specific Jira `field_id` (e.g. `customfield_12345`). |
| `unregister_field_display(field_id)` | Removes the custom render function. |
| `register_field_display_by_name(name, fn)` | Registers a custom render function based on the case-insensitive display name of the field (e.g., `"Progress"`). |
| `unregister_field_display_by_name(name)` | Removes the custom render function by name. |

### `imgui` Module

Smatchet exposes a subset of Dear ImGui functions to allow you to draw custom UI components inside grid cells:

| Function | Description |
| :--- | :--- |
| `imgui.text(str)` | Renders text. |
| `imgui.text_unformatted(str)` | Renders text exactly as-is (best for raw strings). |
| `imgui.progress_bar(fraction, w, h)` | Renders a progress bar. `fraction` is `0.0` to `1.0`. `w` and `h` set the size. Pass `0` for default height. |
| `imgui.get_content_region_avail()` | Returns two numbers `(width, height)` representing the available space in the current cell. |
| `imgui.button("label")` | Renders a clickable button. Returns `true` if clicked this frame. |
| `imgui.same_line()` | Places the next widget on the same line as the previous one. |
| `imgui.separator()` | Draws a horizontal dividing line. |

### `ui` Module

You can construct completely custom dialogs and floating windows directly from Lua.

| Function | Description |
| :--- | :--- |
| `ui.register_window(name, fn)` | Registers a custom floating ImGui window titled `name`. Smatchet will call `fn()` every frame while the window is open. Use the `imgui` module inside `fn` to draw the contents. |
| `ui.register_ticket_action(name, fn)` | Registers a row context-menu action. `name` is the menu label; registering again with the same `name` replaces the callback. `fn(ticket)` receives the row's `Ticket`. See §3. |
| `ui.register_global_action(name, fn)` | Registers a main menu action under **Scripts** (below a separator when any are registered). Same replace-by-`name` semantics. `fn()` takes no arguments. The **Scripts** menu is available whenever the app is built with Lua automation. See §3. |

### `ai` Module

Hooks into the built-in Smatchet AI Assistant.

| Function | Description |
| :--- | :--- |
| `ai.add_context(text)` | Appends text to the AI's session context memory. |
| `ai.prompt(message)` | Programmatically triggers the AI Assistant to process a message, using the current context. |
| `ai.clear_context()` | Clears the accumulated session context. |

### `mcp` Module

Smatchet acts as a Model Context Protocol (MCP) server. You can use Lua to define custom tools that connected AI agents can discover and execute.

| Function | Description |
| :--- | :--- |
| `mcp.register_tool(schema_table, fn)` | Registers an MCP Tool. `schema_table` must contain `name`, `description`, and either `parameters` (as a Lua table) or `parameters_json` (as a JSON string). `fn` receives the arguments as a Lua table and must return a string (or a table which will be JSON-encoded). |

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

---

## 3. Writing Scripts

### Automation Scripts (`Automation.lua`)

You can edit `Automation.lua` directly inside Smatchet using the built-in Lua Console editor.
When you click **"Save & Run on Selected Rows"**, Smatchet compiles your script and invokes the `process_ticket(ticket)` function **only for the tickets you have currently selected** in the active project grid (e.g., via Shift+Click or Ctrl+Click).

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

-- Define our render handler
local function render_progress_bar(issue_id, field_id, raw, avail_width, read_only, field_name)
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
    
    -- Draw an ImGui progress bar
    imgui.progress_bar(fraction, avail_width, 0)
    
    return true -- Tell Smatchet we handled the drawing
end

-- Register the handler to trigger for any field named "Progress"
register_field_display_by_name("Progress", render_progress_bar)
```

### Context menu and menu bar actions (`SmatchetHooks.lua`)

Register these from the same setup script you use for field display (typically `SmatchetHooks.lua`). After editing, use **Save && Reload hooks** on the **SmatchetHooks.lua** tab in **Lua & Automation** so registrations apply.

**Ticket actions** — Right-click **any cell** on a ticket row: **Copy** and **Lua Actions** apply to that issue. **Quick comment templates** still appear only when you right-click the **issue key** column. **Shift+right-click** opens the raw-value popup; Lua ticket actions are listed there as well.

**Global actions** — Use the dockable **Scripts** window (or main menu **Scripts** → your action). Runs with no selected row; use `smatchet.get_active_tickets()` for bulk work. Reopen the panel from **Scripts** → **Scripts panel...** if you closed it.

Callbacks share the same instruction budget as other one-shot Lua work (see §4). There is no dedicated `ai` Lua module for prompts or context; use `log_info`, MCP tools, or automation flows separately.

**Example — ticket row (log description snippet):**

```lua
ui.register_ticket_action("Log description preview", function(ticket)
    local desc = ticket:get_field("description") or ""
    if #desc > 200 then
        desc = desc:sub(1, 200) .. "..."
    end
    log_info(ticket.id .. " description: " .. desc)
end)
```

**Example — global bulk field edit:**

`assignee` values must be valid for Jira (often a JSON user payload). There is no `smatchet.get_current_user()` helper yet; substitute your account id or fetch it outside Lua.

```lua
ui.register_global_action("Bulk set priority (example)", function()
    local tickets = smatchet.get_active_tickets()
    for _, t in ipairs(tickets) do
        local ok, err = t:set_field("priority", "High")
        if not ok then
            log_info(t.id .. " failed: " .. err)
        end
    end
end)
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

### AI-Driven Context & Prompts

You can use Lua to feed relevant context to the AI or trigger specific summaries.

```lua
-- Register an action to summarize a ticket
ui.register_ticket_action("AI: Summarize Context", function(ticket)
    ai.clear_context()
    ai.add_context("The user wants a technical summary of this ticket.")
    ai.add_context("Ticket ID: " .. ticket.id)
    ai.add_context("Current Description: " .. (ticket:get_field("description") or "N/A"))
    
    ai.prompt("Summarize this ticket in 2 concise sentences for a developer.")
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
    - Ticket context-menu actions, global **Scripts** menu actions, MCP tool callbacks, and automation window Lua windows use **100,000 instructions** per invocation (same hook as setup-style runs).
    - Ticket processing (`process_ticket`) and UI rendering handlers are limited to **10,000 instructions**.
    If a script exceeds this limit (e.g., via `while true do end`), it will be immediately aborted and an error will be printed to the Lua Console.
