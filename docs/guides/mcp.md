<!-- tier: project -->
# Smatchet MCP (Model Context Protocol) Guide

Smatchet includes a built-in **Model Context Protocol (MCP)** server. This allows AI agents (like Claude Desktop, cursor, or custom IDE plugins) to connect directly to your running Smatchet instance to discover tools, search Jira tickets, and perform automated actions on your behalf.

---

## 1. Overview

The MCP server in Smatchet acts as a bridge between your local Jira data and external AI models. It provides:
- **Real-time visibility**: AI can see exactly what tickets you have loaded in your grid.
- **Bi-directional interaction**: AI can trigger Jira actions (edits, transitions) via custom tools.
- **Dynamic Extensibility**: You can define new "AI tools" on-the-fly using Lua.

---

## 2. Configuration & Setup

### Enabling the Server
1. Open Smatchet.
2. Go to **File** → **Preferences**.
3. Locate the **MCP (Model Context Protocol)** section.
4. Check **Enable MCP server**.
5. (Optional) Set a custom **MCP Port** (default is `42360`).
6. (Optional) Set an **MCP auth token** for security.
7. (Optional, default off) Enable **Allow MCP run_lua tool (dangerous)** if you want MCP clients to execute Lua snippets/scripts.

### Connecting an AI Client
Smatchet uses the **SSE (Server-Sent Events)** transport for MCP.

To connect a client (like Claude Desktop), use the following configuration pattern:

**Claude Desktop Config (`claude_desktop_config.json`):**
```json
{
  "mcpServers": {
    "smatchet": {
      "url": "http://127.0.0.1:42360/mcp/sse"
    }
  }
}
```

> [!NOTE]
> If you have set an authentication token in Smatchet, the client must include the `X-Smatchet-Token` header in its requests. Some standard MCP clients may require a proxy or custom shim to support this header.

---

## 3. Built-in Tools

Smatchet exposes several core tools to connected AI agents by default:

| Tool Name | Description | Parameters |
| :--- | :--- | :--- |
| `list_active_tickets` | Returns a list of all Jira issue keys currently loaded in Smatchet's project grid. | None |
| `search_active_tickets` | Searches for a specific string within the IDs and field values of all loaded tickets. | `query` (string) |
| `run_lua` (opt-in) | Executes sandboxed Lua from MCP. Hidden unless **Allow MCP run_lua tool (dangerous)** is enabled. | `mode` (`snippet`/`script`), plus `code` or `script`, optional `args` object |

In addition, **every command in the unified command registry auto-publishes as an MCP tool** (its `inputSchema` is generated from the command's param specs). This includes the `view.*` (saved-view CRUD) and `pane.*` (grid-pane scripting) groups — e.g. `pane.list`, `pane.focus` (`id`), `pane.next` / `pane.prev`, `pane.new` / `pane.duplicate` / `pane.split` (`direction?`), `pane.close` (`id?`), and `pane.rename` (`title`, `id?`). Call `tools/list` against your running instance for the live, schema-complete set.

> **Destructive tools require explicit confirmation.** MCP is a non-UI automation source: authenticating with the loopback token grants *reach*, not *blanket destructive authority*. A destructive tool (e.g. `view.delete`, `sync.full`, `app.quit`) returns the structured envelope `{"ok":false,"error":{"code":"confirm-required"}}` unless the `arguments` object carries `"__confirm": true` — a deliberate per-call flag the server never auto-sets. Pass `"__dry_run": true` to preview without mutating. Every destructive call from MCP is audit-logged server-side (security audit 2026-06-13 #2/#3).

---

## 4. Custom Tools via Lua

You can extend the AI's capabilities by registering custom tools in `SmatchetHooks.lua`. This is the most powerful way to automate studio-specific workflows.

### Registration Example
Use `mcp.register_tool(schema, callback)` to define a new tool.

```lua
-- SmatchetHooks.lua
mcp.register_tool({
    name = "set_ticket_priority",
    description = "Changes the priority of a specific Jira ticket.",
    parameters_json = [[
        {
            "type": "object",
            "properties": {
                "ticket_id": { "type": "string", "description": "The Jira key, e.g. PROJ-123" },
                "priority": { "type": "string", "enum": ["Highest", "High", "Medium", "Low", "Lowest"] }
            },
            "required": ["ticket_id", "priority"]
        }
    ]]
}, function(params_json)
    local params = decode_json(params_json)
    local ticket, err = smatchet.get_ticket(params.ticket_id)
    
    if not ticket then
        return '{"status": "error", "message": "Ticket not found."}'
    end
    
    local ok, edit_err = ticket:set_field("priority", params.priority)
    if not ok then
        return '{"status": "error", "message": "' .. edit_err .. '"}'
    end
    
    return '{"status": "success", "message": "Priority updated for ' .. params.ticket_id .. '"}'
end)
```

---

## 5. API Endpoints (Technical Reference)

If you are building a custom integration or debugging, the following endpoints are available:

| Endpoint | Method | Description |
| :--- | :--- | :--- |
| `/mcp/sse` | `GET` | The primary SSE connection endpoint. Returns an `endpoint` event containing the message path. |
| `/mcp/messages` | `POST` | The JSON-RPC endpoint where MCP protocol messages are sent. |
| `/mcp/list_tickets` | `GET` | A simple JSON export of all tickets in the current grid. |
| `/mcp/search?q=...` | `GET` | Simple search endpoint for quick queries. |
| `/mcp/attachment_proxy?url=...` | `GET` | Proxies Jira attachment downloads using Smatchet's credentials. Useful for embedding images in external browsers/tools. |

---

## 6. Security

- **Loopback Only**: By default, the MCP server only listens on `127.0.0.1`. It will reject any request from outside your machine.
- **LAN Access**: If you enable **Bind on all interfaces (LAN)** in Preferences, the server will listen on `0.0.0.0`. 
- **Authentication**: We strongly recommend setting an **Auth Token** if LAN access is enabled. Clients must provide this token via the `X-Smatchet-Token` HTTP header.
- **Instruction Limits**: All custom Lua tool callbacks are subject to a **100,000 instruction limit** to prevent AI-triggered infinite loops or hangs.
- **run_lua Gating**: `run_lua` is disabled by default and only appears in `tools/list` after you enable the dangerous opt-in in Preferences -> Integrations. Saving Preferences restarts MCP so the toggle applies immediately.

---

## 7. Troubleshooting

- **Server won't start**: Ensure the port (default `42360`) isn't being used by another application (like a local web server or another dev tool). Check the **MCP Activity Log** in Smatchet (**Automation** -> **Agent Bridge (MCP)...**) for error messages.
- **Connection Refused**: Check your firewall settings. If the client is on the same machine, ensure it's trying to connect to `127.0.0.1` and not your external IP (unless LAN access is enabled).
- **Tools not appearing**: Ensure your `SmatchetHooks.lua` script is saved and you have used **Save and Reload Hooks** in **Automation -> Scripts & Actions...** (`SmatchetHooks.lua`).
