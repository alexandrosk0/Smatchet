-- Edit in Windows → Scripting… (pick SmatchetHooks.lua); use Save && Reload Hooks after edits.
-- Registers UI hooks: field renderers, ticket actions, global quick actions, custom windows, MCP tools, etc.
--
-- Two ways to register a cached cell renderer:
--   register_field_display_cached("customfield_12345", fn)        -- Jira field id (see catalog / column key)
--   register_field_display_cached_by_name("Progress", fn)         -- display name, case-insensitive
--
-- Function signature (7 args — provider returns a recorded draw list, called only on cache miss):
--   function(issue_id, field_id, raw, avail_width, read_only, field_name, draw) -> boolean
--
-- The `draw` arg is a recorder: methods queue opcodes (draw:text, draw:image, draw:button, ...)
-- that Smatchet replays each frame in C++ until one of the cache-key inputs changes
-- (raw / field_name / rounded avail_width / read_only / provider generation). Net effect:
-- Lua runs ~once per cell per refresh, not per frame — about 50-60× cheaper than the legacy
-- register_field_display path. Direct imgui.* calls inside a cached provider luaL_error;
-- use draw:* instead (the recorder is needed so replay sees the same ops).
--
-- Recorder ops (v1):
--   Static draw:    draw:text(s), draw:text_unformatted(s), draw:image(path, w, h),
--                   draw:progress_bar(frac, w, h, overlay?), draw:same_line(off?, sp?),
--                   draw:separator(), draw:dummy(w, h), draw:push_color(idx, r, g, b, a),
--                   draw:pop_color(n?), draw:set_tooltip(s)
--   Interactive:    draw:button(label, fn(ticket_id, field_id))
--                   draw:input_text(label, initial, max_len, fn(ticket_id, field_id, new_value))
--   Post-widget:    draw:on_deactivated(fn), draw:on_deactivated_after_edit(fn)
--                   (attach to the most-recently-recorded interactive op)
--
-- Custom windows (recorder shape):
--   ui.register_window("My panel", function(draw) draw:text("hello") end)
--   ui.invalidate_window("My panel")            -- force re-record on next frame
--   ui.unregister_window("My panel")
--
-- Explicit cell-cache invalidation (needed when your provider reads Lua state outside its 6 args):
--   ui.invalidate_field_cache()                                     -- drop entire cache
--   ui.invalidate_field_cache_for(ticket_id)                        -- drop one ticket
--   ui.invalidate_field_cache_for(ticket_id, field_id)              -- drop one cell
--
-- Field icons (priority + custom) — these run AFTER the cached provider returns false:
--   register_field_icon_map(field_id, { ["high"] = "C:/path/icon.png", medium = "https://..." })
--     Read-only grid: icon replaces text (hover for value). Editable SingleSelect: icon replaces closed-combo preview text.
--     Built-in priority also uses shipped Scripts/art/priority/<slug>.png when present (offline), else Jira iconUrl / domain fetch.
--     Keys: normalized option text (lowercase). Values: absolute path, path under Scripts/,
--     or https URL (fetched with size cap + disk cache).
--   register_field_icon_map("My Field", { ... }, true)  -- third arg true = match by display name
--   unregister_field_icon_map("priority")
--   unregister_field_icon_map("My Field", true)
--
--   draw:image(path_or_url, width, height)  -- use inside register_field_display_cached for full custom UI
--





local function render_progress_json(issue_id, field_id, raw, avail_width, read_only, field_name, draw)
  -- decode_json on a hot Lua path can be costly + can throw a C++ parse_error past the
  -- protected call on certain malformed inputs (PR #54 follow-up). Prefer string.match;
  -- only the user-visible "fraction" is needed here.
  if not raw or raw == "" then return false end
  local p = tonumber(raw:match('"progress"%s*:%s*(%-?%d+%.?%d*)')) or 0
  local t = tonumber(raw:match('"total"%s*:%s*(%-?%d+%.?%d*)')) or 0
  local frac = (t > 0) and (p / t) or 0
--draw:text("Test ")
draw:text(string.format("%d%%", math.floor(frac * 100)))
  --draw:progress_bar(frac, avail_width, 0, string.format("%d%%", math.floor(frac * 100)))
  return true
end

-- Match your Jira field *display name* (e.g. "Progress"). Case does not matter.
register_field_display_cached_by_name("progress", render_progress_json)


log_info("Automation loaded2")



