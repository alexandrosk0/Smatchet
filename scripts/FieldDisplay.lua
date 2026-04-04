-- Click "Reload FieldDisplay.lua" after edits.
--
-- Two ways to register:
--   register_field_display("customfield_12345", fn)     -- Jira field id (see catalog / column key)
--   register_field_display_by_name("Progress", fn)      -- display name, case-insensitive
--
-- ctx: issue_id, field_id, raw, avail_width, read_only, field_name (or nil)
--

--[[ 
local function render_progress_json(ctx)
  local j, err = decode_json(ctx.raw)
  if not j then
    return false
  end
  local p, t = tonumber(j.progress) or 0, tonumber(j.total) or 0
  local frac = (t > 0) and (p / t) or 0
  imgui.progress_bar(frac, ctx.avail_width, 0)
  return true
end

-- Match your Jira field *display name* (e.g. "Progress"). Case does not matter.
register_field_display_by_name("progress", render_progress_json)

-- If you prefer the raw id instead, comment the line above and use:
-- register_field_display("customfield_XXXXX", render_progress_json)

log_info("FieldDisplay.lua loaded (progress renderer registered by name)")
]]