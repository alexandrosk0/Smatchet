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
  draw:progress_bar(frac, avail_width, 0, string.format("%d%%", math.floor(frac * 100)))
  return true
end

-- Match your Jira field *display name* (e.g. "Progress"). Case does not matter.
register_field_display_cached_by_name("progress", render_progress_json)

-- If you prefer the raw id instead, comment the line above and use:
-- register_field_display_cached("customfield_XXXXX", render_progress_json)

log_info("SmatchetHooks.lua loaded (progress renderer registered by name)")


--[[ Field icons — remove the surrounding --[[ and  to try this (or paste into an uncommented section).
register_field_icon_map("priority", {
  blocker = "Scripts/art/priority/blocker.png",
})
-- Custom single-select field by id (separate call per field):
-- register_field_icon_map("customfield_10001", { open = "C:/path/open.png", ["in progress"] = "C:/path/wip.png" })

-- Example: icon from disk inside a full custom field renderer
-- register_field_display_cached("customfield_99999", function(issue_id, field_id, raw, avail_width, read_only, field_name, draw)
--   draw:image("Scripts/art/badge.png", 18, 18)
--   draw:same_line()
--   draw:text(raw or "")
--   return true
-- end)
]]

-- =============================================================================
-- Priority field renderer using the recorded-cmd-list cached path.
--
-- Replaces the C++ priority SingleSelect path with an icon-only Lua renderer.
-- Trade-off: the cell is no longer click-to-edit (the C++ combo path is bypassed
-- entirely). Use the issue detail panel to change priority.
--
-- Performance design with the cached recorder:
--   1. Lua runs ONCE per cell per refresh (the recorded `draw:image` op is replayed
--      directly by C++ on every subsequent frame until raw / avail_width / read_only
--      changes). With the legacy register_field_display path, Lua ran every frame.
--   2. SLUG_TO_PATH is built ONCE at script load. No string concatenation per cell.
--   3. PRIORITY_MEMO is a per-rawValue Lua-side cache that survives the rare cache miss
--      when a cell re-records — saves the slug normalize step on a refresh.
--   4. String pattern matching instead of decode_json:
--        - Avoids the sol2 marshalling round-trip (string → C++ → JsonToLua table → Lua).
--        - Avoids nlohmann allocation + JsonToLua table-build (typically ~5-10 µs/call).
--        - `string.match` is a single O(len) pattern scan with one string allocation.
--        - Also sidesteps a pre-existing crash where decode_json's C++ parse_error can
--          escape try/catch on certain malformed inputs (see Smatchet PR #54 follow-up).
--   5. We always return true — even for empty/unknown values — so the C++
--      SingleSelect editor never runs for priority cells.
--
-- Disable by commenting out the register_field_display_cached call at the bottom.
-- =============================================================================

local PRIORITY_ICON_DIR  = "Scripts/art/priority/"
local PRIORITY_ICON_SIZE = 18  -- px; matches ImGui::GetFrameHeight() at default font.

-- Pre-built slug → bundled-PNG table. Constant after script load.
local SLUG_TO_PATH = {
  blocker  = PRIORITY_ICON_DIR .. "blocker.png",
  critical = PRIORITY_ICON_DIR .. "critical.png",
  high     = PRIORITY_ICON_DIR .. "high.png",
  highest  = PRIORITY_ICON_DIR .. "highest.png",
  low      = PRIORITY_ICON_DIR .. "low.png",
  lowest   = PRIORITY_ICON_DIR .. "lowest.png",
  major    = PRIORITY_ICON_DIR .. "major.png",
  medium   = PRIORITY_ICON_DIR .. "medium.png",
  minor    = PRIORITY_ICON_DIR .. "minor.png",
  trivial  = PRIORITY_ICON_DIR .. "trivial.png",
}

-- Per-rawValue memo. Built lazily, persists for the lifetime of the Lua state.
-- Values are either an icon path string OR `false` (sentinel: no icon, don't retry).
local PRIORITY_MEMO = {}

-- Hoist commonly-used functions to locals (saves a global-env lookup per call).
local string_lower = string.lower
local string_gsub  = string.gsub
local string_match = string.match

-- Normalize a priority display name to a slug key.
-- "In Progress" → "inprogress",  "Won't Fix" → "wontfix",  "High" → "high".
local function normalize_slug(s)
  if not s or s == "" then return nil end
  local lower = string_lower(s)
  lower = string_gsub(lower, "[%s%-_'%.]", "")
  return lower
end

-- Resolve a rawValue (Jira priority JSON or plain text label) to an icon path or nil.
-- Uses string.match instead of decode_json — faster and avoids the parse_error
-- escape that crashes the process when raw is not valid JSON (e.g. plain "High").
local function resolve_priority_icon(raw)
  if not raw or raw == "" then return nil end

  -- Try to pull out a JSON "name" field: {"name":"High", "id":"3", ...}
  -- The pattern is permissive about whitespace and order. Single allocation.
  local name = string_match(raw, '"name"%s*:%s*"([^"]+)"')
  if name then
    local slug = normalize_slug(name)
    if slug then
      local p = SLUG_TO_PATH[slug]
      if p then return p end
    end
  end

  -- Plain-text fallback: rawValue might already be the label "High" / "Medium".
  local slug = normalize_slug(raw)
  if slug then
    local p = SLUG_TO_PATH[slug]
    if p then return p end
  end

  return nil
end

-- Cell record handler — 7th arg `draw` records ops; C++ replays each frame.
-- Declared globally (no `local`) so the `priority-grid-scroll` scenario's
-- `registerLuaProvider` arg can bind it via `_G.render_priority_cell` for
-- automated perf measurement without manual script edits.
function render_priority_cell(issue_id, field_id, raw, avail_width, read_only, field_name, draw)
  -- Indexing with nil errors in Lua; use empty string as a stable key.
  local key = raw or ""
  local cached = PRIORITY_MEMO[key]
  if cached ~= nil then
    if cached then
      draw:image(cached, PRIORITY_ICON_SIZE, PRIORITY_ICON_SIZE)
    end
    return true
  end
  local path = resolve_priority_icon(key)
  PRIORITY_MEMO[key] = path or false
  if path then
    draw:image(path, PRIORITY_ICON_SIZE, PRIORITY_ICON_SIZE)
  end
  return true
end

-- IMPORTANT — performance note (measured on Smatchet PR #54+ codebase):
--   With the recorded-cmd-list path, Lua runs once per cell per refresh, not per frame.
--   Replay of `draw:image(...)` is ~5 µs / cell vs ~390 µs / cell for the legacy
--   register_field_display path (~50-60× cheaper). The C++ priority renderer (~6.7 µs / cell)
--   is still slightly faster than replay, but only by a constant factor — and the cached Lua
--   path lets you customise (different icons, decoration, click handler) without paying the
--   per-frame Lua cost the legacy path imposed.
--
-- Bottom line: enable by uncommenting the register_field_display_cached line below if you
-- need custom priority rendering. The C++ default remains the absolute fastest path.
--
-- register_field_display_cached("priority", render_priority_cell)

log_info("SmatchetHooks.lua: priority Lua cached handler available but DISABLED by default (see perf note above)")

-- =============================================================================

-- Example: Data Migration between backends (e.g. Jira -> Plane)
-- 1. Load your issues from Jira.
-- 2. Switch to Plane in Preferences and Save & Sync.
-- 3. Run this action from the Lua Console (Tools & Actions tab).
function migrate_to_active_backend()
  local tickets = smatchet.get_active_tickets()
  local backend = tracker.get_type()
  log_info("Migrating " .. #tickets .. " tickets to " .. backend)
  
  for _, t in ipairs(tickets) do
    local fields = {
      summary = t:get_field("summary") or "No Summary",
      description = t:get_field("description") or "",
      priority = (t:get_field("priority") or "medium"):lower()
    }
    
    -- Add more field mappings as needed
    local key, err = tracker.create_issue(fields)
    if key ~= "" then
      log_info("Migrated: " .. t.id .. " -> " .. key)
    else
      log_info("Failed: " .. t.id .. " Error: " .. err)
    end
  end
end

ui.register_global_action("Migrate to Active Backend", "migrate_to_active_backend")

log_info("SmatchetHooks.lua: Migration tool registered.")

