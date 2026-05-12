-- Edit in Windows → Scripting… (pick SmatchetHooks.lua); use Save && Reload Hooks after edits.
-- Registers UI hooks: field renderers, ticket actions, global quick actions, custom windows, MCP tools, etc.
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

--[[ Field icons — remove the surrounding --[[ and  to try this (or paste into an uncommented section).
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

-- =============================================================================
-- Optimized priority field renderer in Lua.
--
-- Replaces the C++ priority SingleSelect path with an icon-only Lua renderer.
-- Trade-off: the cell is no longer click-to-edit (the C++ combo path is bypassed
-- entirely). Use the issue detail panel to change priority. In exchange, the
-- per-cell render cost drops to ~one hashmap probe + one imgui.image() call.
--
-- Performance design (every choice here is per-cell-hot, beware):
--   1. SLUG_TO_PATH is built ONCE at script load. No string concatenation per cell.
--   2. PRIORITY_MEMO is a per-rawValue cache. Lua tables are O(1) hash maps;
--      `false` sentinel marks "we tried this rawValue, no icon" so we don't
--      re-resolve every frame.
--   3. String pattern matching instead of decode_json:
--        - Avoids the sol2 marshalling round-trip (string → C++ → JsonToLua table → Lua).
--        - Avoids nlohmann allocation + JsonToLua table-build (typically ~5-10 µs/call).
--        - `string.match` is a single O(len) pattern scan with one string allocation.
--        - Also sidesteps a pre-existing crash where decode_json's C++ parse_error can
--          escape try/catch on certain malformed inputs (see Smatchet PR #54 follow-up).
--   4. We always return true — even for empty/unknown values — so the C++
--      SingleSelect editor never runs for priority cells. This saves the
--      BeginCombo widget cost (~2-5 µs × 18 cells = 30-90 µs / frame).
--   5. Hoist commonly-used functions to locals at script-load time. Local-variable
--      access in Lua is a register-indexed slot; globals hit the env table.
--
-- Disable by commenting out the register_field_display call at the bottom.
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
local imgui_image  = imgui.image
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

-- Cell render handler. Engine fixes the signature — positional args avoid a
-- varargs-table allocation on the dispatch hot path.
local function render_priority_cell(issue_id, field_id, raw, avail_width, read_only, field_name)
  -- Indexing with nil errors in Lua; use empty string as a stable key.
  local key = raw or ""
  local cached = PRIORITY_MEMO[key]
  if cached ~= nil then
    -- Fast path: one hashmap probe + (optional) one imgui.image call.
    if cached then
      imgui_image(cached, PRIORITY_ICON_SIZE, PRIORITY_ICON_SIZE)
    end
    return true
  end
  -- Cold path: resolve once, memoise the result (path-string OR false sentinel).
  local path = resolve_priority_icon(key)
  PRIORITY_MEMO[key] = path or false
  if path then
    imgui_image(path, PRIORITY_ICON_SIZE, PRIORITY_ICON_SIZE)
  end
  return true
end

-- IMPORTANT — performance note (measured on Smatchet PR #54+ codebase):
--   The C++ priority renderer in develop is already heavily optimised (round-3 PR):
--   ~0.12 ms total for 18 visible priority cells (≈ 6.7 µs / cell).
--
--   The Lua handler above is functionally equivalent but ~50-60× SLOWER per cell
--   because of fixed sol2 dispatch overhead per call:
--     - 6 marshalled args (3 std::string copies, fieldName sol::object, bool, float)
--     - per-call lua_sethook for instruction-count timeout protection
--     - sol::protected_function call + return-value marshalling
--   Measured: 19.6 ms / 50 cells = ~390 µs / cell with Lua, vs 6.7 µs / cell C++.
--
--   register_field_icon_map (which routes the per-cell work back through C++
--   instead of dispatching to Lua) is also slower than the optimised path:
--   ~16 ms / 50 cells = ~320 µs / cell, because TryGetFieldIconMapTarget activates
--   a heavier resolution path (ResolveOptionId + slug normalisation + key vector)
--   that the default priority renderer doesn't pay.
--
-- Bottom line: leave the Lua hook DISABLED for priority unless you need full
-- customisation (different icons, additional decoration, click handler, etc.).
-- Enable by uncommenting the register_field_display line below.
--
register_field_display("priority", render_priority_cell)

log_info("SmatchetHooks.lua: priority Lua renderer ENABLED")

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
