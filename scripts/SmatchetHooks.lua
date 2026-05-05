-- Edit in Lua & Automation → SmatchetHooks.lua tab; use "Save && Reload hooks" after edits.
-- Registers UI hooks: field renderers, ticket actions, global Scripts actions, custom windows, MCP tools, etc.
--
-- Two ways to register field display:
--   register_field_display("customfield_12345", fn)     -- Jira field id (see catalog / column key)
--   register_field_display_by_name("Progress", fn)      -- display name, case-insensitive
--
-- Function signature:
--   function(issue_id, field_id, raw, avail_width, read_only, field_name) -> boolean
--
-- Field icons (priority + custom):
--   register_field_display / register_field_display_by_name run first; if they return true,
--   built-in priority icons and register_field_icon_map are skipped (Lua wins).
--
--   register_field_icon_map(field_id, { ["high"] = "C:/path/icon.png", medium = "https://..." })
--     Read-only grid: icon replaces text (hover for value). Editable SingleSelect: icon replaces closed-combo preview text.
--     Built-in priority also uses shipped Scripts/art/priority/<slug>.png when present (offline), else Jira iconUrl / domain fetch.
--     Keys: normalized option text (lowercase). Values: absolute path, path under Scripts/,
--     or https URL (fetched with size cap + disk cache).
--   register_field_icon_map("My Field", { ... }, true)  -- third arg true = match by display name
--   unregister_field_icon_map("priority")
--   unregister_field_icon_map("My Field", true)
--
--   imgui.image(path_or_url, width, height)  -- use inside register_field_display for full custom UI
--

--[[ 
local function render_progress_json(issue_id, field_id, raw, avail_width, read_only, field_name)
  local j, err = decode_json(raw)
  if not j then
    return false
  end
  local p, t = tonumber(j.progress) or 0, tonumber(j.total) or 0
  local frac = (t > 0) and (p / t) or 0
  imgui.progress_bar(frac, avail_width, 0)
  return true
end

-- Match your Jira field *display name* (e.g. "Progress"). Case does not matter.
register_field_display_by_name("progress", render_progress_json)

-- If you prefer the raw id instead, comment the line above and use:
-- register_field_display("customfield_XXXXX", render_progress_json)

log_info("SmatchetHooks.lua loaded (progress renderer registered by name)")
]]

--[[ Field icons — remove the surrounding --[[ and  to try this (or paste into an uncommented section). ]] 
register_field_icon_map("priority", {
  blocker = "Scripts/art/priority/blocker.png",
})
-- Custom single-select field by id (separate call per field):
-- register_field_icon_map("customfield_10001", { open = "C:/path/open.png", ["in progress"] = "C:/path/wip.png" })

-- Example: icon from disk inside a full custom field renderer
-- register_field_display("customfield_99999", function(issue_id, field_id, raw, avail_width, read_only, field_name)
--   imgui.image("Scripts/art/badge.png", 18, 18)
--   imgui.same_line()
--   imgui.text(raw or "")
--   return true
-- end)
]]
