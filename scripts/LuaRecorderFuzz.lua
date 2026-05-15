-- LuaRecorderFuzz.lua
--
-- Hostile-input fuzz provider for the recorded-cmd-list path. Exercises every
-- crash-safety bound in `LuaDrawList::*` recorder methods + `ReplayCmdList`
-- replay handlers. Loaded by the `lua-recorder-fuzz` scenario at OnStart;
-- bound to a configurable field (default `summary`) for the duration of the
-- scenario, then unbound at OnFinish.
--
-- Plan reference: docs/design/applied/lua-recorded-cmd-list.md §Crash-safety hardening
--   §Fuzz test (verification step 14)
--
-- Expected: zero crashes, zero asserts; warn-log churn within rate limits.
-- The whole point is to prove the recorder + replay path tolerate every
-- documented hostile-input class.

local FUZZ_SEED = 1
local function fuzz_rand()
  FUZZ_SEED = (FUZZ_SEED * 1103515245 + 12345) % 2147483648
  return FUZZ_SEED
end

local function fuzz_float()
  local r = fuzz_rand() % 100
  if r < 5  then return 0/0 end             -- NaN
  if r < 10 then return 1/0 end             -- +Inf
  if r < 15 then return -1/0 end            -- -Inf
  if r < 20 then return -1e9 end            -- huge negative
  if r < 25 then return 1e9 end             -- huge positive
  return (fuzz_rand() % 1000) / 10.0
end

local function fuzz_int()
  local r = fuzz_rand() % 100
  if r < 5  then return -1 end
  if r < 10 then return 99999 end           -- way past ImGuiCol_COUNT
  if r < 15 then return -99999 end
  return fuzz_rand() % 60                   -- in valid color range
end

local function fuzz_str()
  local r = fuzz_rand() % 100
  if r < 10 then return "" end              -- empty
  if r < 15 then return string.rep("X", 200000) end  -- huge
  if r < 20 then return "\0\0\0" end        -- embedded NULs
  return "fuzz-" .. tostring(fuzz_rand())
end

function render_fuzz_cell(issue_id, field_id, raw, avail_width, read_only, field_name, draw)
  local pushed = 0
  local op_count = fuzz_rand() % 12 + 1

  for _ = 1, op_count do
    local op = fuzz_rand() % 14

    if op == 0 then
      draw:text(fuzz_str())
    elseif op == 1 then
      draw:text_unformatted(fuzz_str())
    elseif op == 2 then
      draw:image(fuzz_str(), fuzz_float(), fuzz_float())
    elseif op == 3 then
      draw:progress_bar(fuzz_float(), fuzz_float(), fuzz_float(), fuzz_str())
    elseif op == 4 then
      draw:same_line(fuzz_float(), fuzz_float())
    elseif op == 5 then
      draw:separator()
    elseif op == 6 then
      draw:dummy(fuzz_float(), fuzz_float())
    elseif op == 7 then
      draw:push_color(fuzz_int(), fuzz_float(), fuzz_float(), fuzz_float(), fuzz_float())
      pushed = pushed + 1
    elseif op == 8 then
      -- Mostly balanced, occasionally over-pop to exercise the defensive clamp
      local n = (fuzz_rand() % 4) + 1
      draw:pop_color(n)
      pushed = math.max(0, pushed - n)
    elseif op == 9 then
      draw:set_tooltip(fuzz_str())
    elseif op == 10 then
      draw:button(fuzz_str(), function(id, fid)
        -- intentionally no-op; just exercise the callback marshalling
      end)
    elseif op == 11 then
      -- max_len cap stress: sometimes huge, sometimes zero (recorder clamps)
      local maxlen = ({0, -5, 65, 100000})[(fuzz_rand() % 4) + 1]
      draw:input_text(fuzz_str(), fuzz_str(), maxlen, function(id, fid, v)
        -- no-op
      end)
    elseif op == 12 then
      draw:on_deactivated(function() end)
    elseif op == 13 then
      draw:on_deactivated_after_edit(function() end)
    end
  end

  -- Intentionally leave some unbalanced push_color outstanding to exercise
  -- ReplayCmdList's post-loop `if (pushed > 0) PopStyleColor(pushed)` guard.
  return true
end
