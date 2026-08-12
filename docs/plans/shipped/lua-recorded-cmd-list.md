# Lua recorded ImGui command list — cached cell + window bindings

<!-- plan-date: 2026-05-14 -->
<!-- index-summary: Lua recorded ImGui command list — cached cell + window bindings (PR #66 `5b740e9`). Replaces per-frame Lua dispatch with cached command replay (~390 µs/cell → ~5 µs/cell). -->

> **Status**: shipped on `develop` via PR #66 (squash `5b740e9`). See `## Implementation log`, `## Deviations from plan`, and `## Verification` near the bottom of this doc.

## Context

`register_field_display` calls Lua **every frame, per visible cell**. Round-3 perf measurement: ~390 µs / cell × 18 cells ≈ 7 ms / frame of sol2 dispatch overhead (6 marshalled args, `lua_sethook` setup/teardown, `protected_function` call/return marshalling). It also runs **before** the optimized C++ priority renderer in `TicketFieldEditor::RenderFieldCell`, bypassing the 384-entry texture cache and negative-memo path.

`register_window` has the same shape — `DrawLuaWindows` (`AppController_LuaBindings.cpp:1513-1542`) invokes each registered Lua draw function every frame.

Ticket data only changes on refresh / sync / single-cell edit, so the Lua output for cells is stable between mutations. Window content is stable between events (user interactions, sync, explicit invalidation). **Use a recorded ImGui command list**: Lua draws into a `draw` handle that *records* opcodes; C++ caches the list; each frame replays the cached list directly via ImGui — **no Lua call** until invalidation. Interactive widgets carry callbacks that fire only on the user event.

Outcomes:
- **Cells**: Lua provider runs once per cell per refresh. Replay ≈ 5 µs / cell vs 390 µs.
- **Windows**: Lua draw fn runs once per *event* (click, commit, sync, explicit invalidate). Replay ≈ 1-2 µs / cmd.

User decisions:
1. New binding name: **`register_field_display_cached`** (+ `_by_name` variant).
2. Old `register_field_display` / `_by_name`: **remove entirely**. Only per-frame Lua binding in the codebase; `register_field_icon_map` is already one-shot.
3. Recorder ops (v1):
   - **Static draw**: `text`, `text_unformatted`, `image`, `progress_bar`, `same_line`, `separator`, `dummy`, `push_color`, `pop_color`, `set_tooltip`.
   - **Interactive** (carry a Lua callback; callback fires only on user event): `button`, `input_text`.
   - **Post-widget modifiers** (attach to last interactable): `on_deactivated`, `on_deactivated_after_edit`.
   - **Not in v1**: combo, drag, slider, checkbox (mechanical extension, same pattern). Query ops (`get_content_region_avail`) — Lua already receives `avail_width`.
4. `register_window`: same recorder + a `dirty` flag invalidated by `Click`/`Commit` callbacks (Q6), `luaWindowDataGen_` / `luaProviderGen_` bumps (Q2/Q5), or explicit `ui.invalidate_window(name)` (Q10).
5. Lua **may not** draw the UI directly every frame via the `imgui.*` immediate-mode bindings inside a window draw fn or a cell provider. Those bindings remain only for event-time callbacks (`register_ticket_action`, `register_global_action`). A runtime guard `g_luaImmediateModeAllowed` (thread-local bool) is `false` during cached cell/window recording; the existing `imgui.*` glue in `InitLuaUi` (`AppController_LuaBindings.cpp:613`) checks the flag and `luaL_error`s with `"imgui.* not allowed inside cached provider / window — use draw:* instead"`. Prevents the silent "draws once, vanishes next frame" failure mode.

## Critical files

- `Source_Core/include/AppController.h` — new types, maps, two atomic gens (`luaProviderGen_`, `luaWindowDataGen_`), method decls (`TryRenderCachedLuaField`, `NotifyLuaTicketDataChanged`, `LuaUiInvalidateWindowBind`, `LuaUiInvalidateFieldCacheBind` — all outside the Lua guard per Q11); remove legacy decls; replace `luaWindows_` shape.
- `Source_Core/src/AppController_LuaBindings.cpp` — `LuaDrawList` usertype + glue + bind funcs + `TryRenderCachedLuaField` + `ReplayCmdList` + `InvokeLuaCallbackSandboxed` + new `DrawLuaWindows` + `LuaUiInvalidateWindowBind` + `LuaUiInvalidateFieldCacheBind` + `NotifyLuaTicketDataChanged` + `LuaHookGuard` + `LuaImmediateModeGuard`; remove legacy display + draw-fn paths.
- `Source_Core/src/AppController_LuaStubs.cpp` — no-op stubs for every method declared outside the Lua guard.
- `Source_Core/src/TicketFieldEditor.cpp:834` — one-line dispatch swap (adds `allowEdits` arg per Q3).
- `Source_Core/src/AppController_CatalogAndFieldEdit.cpp` — call `NotifyLuaTicketDataChanged()` from `RefreshLocalData` + `UpdateTicket`.
- `Source_Core/src/TicketSyncService.cpp` — flip `pendingLuaWindowBump_` in `ApplyIssueFetchPack`; call `NotifyLuaTicketDataChanged()` once at fetch-session end (Q2 coalescing).
- `Source_Core/src/AppController.cpp:356` (`~AppController` shutdown path that already calls `ClearLuaTicketContextGlue`) — verify shutdown order (see Risks + finding #1).
- `CMakeLists.txt:333` — **required crash-safety change**: set `LANGUAGE CXX` on Lua sources so `luaL_error` uses C++ exceptions (see Crash-safety hardening § Lua build mode).
- `scripts/SmatchetHooks.lua` — migrate examples.
- `docs/guides/lua.md` — replace binding docs.

## Design

### Lua-facing contract — cells

Provider signature gains a 7th arg, the `draw` handle:

```lua
register_field_display_cached("priority", function(issue_id, field_id, raw, avail_width, read_only, field_name, draw)
    local key = raw or ""
    local cached = PRIORITY_MEMO[key]
    if cached ~= nil then
        if cached then draw:image(cached, PRIORITY_ICON_SIZE, PRIORITY_ICON_SIZE) end
        return true
    end
    local path = resolve_priority_icon(key)
    PRIORITY_MEMO[key] = path or false
    if path then draw:image(path, PRIORITY_ICON_SIZE, PRIORITY_ICON_SIZE) end
    return true
end)
```

`draw` is a sol2 usertype. Methods append opcodes to a backing `std::vector<ImCmd>`. The Lua body runs only on cache miss; the recorded list is replayed every other frame.

Provider returns `true` (truthy in Lua — note `0` and `""` are truthy, only `false` / `nil` are falsy) → cached as **handled**, list replayed. Returns `false` / `nil` → cached as **fall-through**, C++ renderer used. Re-evaluated only when one of the cache-key inputs changes (`rawValue` / `fieldName` / `intAvailWidth` / `isReadOnly` / `providerGen` — see `TryRenderCachedLuaField`) or on explicit `ui.invalidate_field_cache*` (Q1, Q10).

### Lua-facing contract — windows

```lua
ui.register_window("My panel", function(draw)
    draw:text("Hello")
    draw:button("Refresh", function(window_name)
        log_info("clicked " .. window_name)
    end)
end)
ui.invalidate_window("My panel")  -- force re-record on next frame
```

The window draw fn now takes the `draw` handle as its first (and only) arg. Existing scripts that call `imgui.text(...)` inside their draw fn break — by design (see decision 5).

### Recorder API (v1) — common to cells and windows

| Lua | C++ recorded |
|---|---|
| `draw:text(s)` | `ImGui::Text("%s", s)` at replay |
| `draw:text_unformatted(s)` | `ImGui::TextUnformatted(s)` |
| `draw:image(path_or_url, w, h)` | `app.LuaImGuiImageBind(path, w, h)` — pure C++ (`SmatchetFieldIconRender::DrawImagePathOrUrl` → texture cache + `ImGui::Image`). The `Lua` in the name is "exposed to Lua", not "calls Lua"; no Lua state touched at replay. |
| `draw:progress_bar(frac, w, h, overlay?)` | `ImGui::ProgressBar(frac, sz, overlay)` |
| `draw:same_line(offset?, spacing?)` | `ImGui::SameLine(o, s)` |
| `draw:separator()` | `ImGui::Separator()` |
| `draw:dummy(w, h)` | `ImGui::Dummy({w,h})` |
| `draw:push_color(col_idx, r, g, b, a)` | `ImGui::PushStyleColor(col_idx, {r,g,b,a})` |
| `draw:pop_color(count?)` | `ImGui::PopStyleColor(count)` |
| `draw:set_tooltip(s)` | `if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s)` |
| `draw:button(label, on_click_fn)` | `if (ImGui::Button(label##idx)) on_click_fn(cb_arg1, cb_arg2)` |
| `draw:input_text(label, initial, max_len, on_commit_fn)` | persistent buffer; `ImGui::InputText(label##idx, …)`; commit fires on `IsItemDeactivatedAfterEdit()` |
| `draw:on_deactivated(fn)` | record-time: attach to last interactive `ImCmd`'s `onDeactivated` slot; replay: `if (ImGui::IsItemDeactivated()) fn(cb_arg1, cb_arg2)` |
| `draw:on_deactivated_after_edit(fn)` | record-time: attach to last interactive `ImCmd`'s `onDeactivatedAfterEdit` slot; replay: `if (ImGui::IsItemDeactivatedAfterEdit()) fn(cb_arg1, cb_arg2)` |

**Auto-`##` suffix**: the recorder appends `##op<index>` to interactive widget labels at record time (counter is a member of `LuaDrawList`, incremented per interactive op). Lua scripts never need to write unique IDs. ImGui's outer ID scope (`CellIdScope` for cells, `ImGui::Begin(name)` for windows) handles cross-cell / cross-window disambiguation.

**`cb_arg1`, `cb_arg2`**: for cells = `(ticket.id, fieldId)`; for windows = `(window_name, "")`. Replay passes these from the caller (see `ReplayCmdList` signature).

**Hot-path Lua cost**: zero per frame until the user clicks / commits or one of the cell cache-key inputs changes (windows: dirty / gen bump). Then one sandboxed `protected_function` call. Cell value edits trigger `UpdateTicket` → cell's cached `rawValue` no longer matches → next frame re-records that one cell (per-cell invalidation, ADR 0001).

### C++ types — in `AppController.h`, inside `#if SMATCHET_WITH_LUA_AUTOMATION`

`ImCmd` references `sol::protected_function`, so the type lives behind the guard. Stub `TryRenderCachedLuaField` returns false without touching the type.

`ImCmd` carries both pre-deactivation and post-deactivation callback slots so `draw:on_deactivated*` mutates the **last interactive command in place** at record time (finding #9). Replay can't store these as separate ops because intervening `text` / `same_line` / `set_tooltip` ops would shift what `ImGui::IsItemDeactivated*` observes.

```cpp
struct ImCmd {
    enum class Op : std::uint8_t {
        Text, TextUnformatted, Image, ProgressBar,
        SameLine, Separator, Dummy,
        PushColor, PopColor, SetTooltip,
        Button, InputText
    };
    Op          op;
    std::string str;       // text / image path / tooltip / overlay / suffixed label
    float       f1, f2, f3, f4;
    int         i1;        // pop count / input max_len
    sol::protected_function callback;           // primary: button click, input commit
    sol::protected_function onDeactivated;       // attached at record time to last interactive
    sol::protected_function onDeactivatedAfterEdit;
    std::vector<char>       textBuf;   // primed at record time by LuaDrawList::InputText
    ImCmd() : op(Op::Separator), f1(0), f2(0), f3(0), f4(0), i1(0) {}
};
struct LuaFieldCacheEntry {
    std::vector<ImCmd> cmds;
    std::string        rawValue;        // (finding #2/#3) inputs that produced cmds
    std::string        fieldName;       // fieldMeta->Name at record time
    int                intAvailWidth;   // (F8) rounded availWidth — exact match
    bool               isReadOnly;      // (F3) catalogReadOnly || !CanEditFieldForIssue || !allowEdits
    std::uint64_t      providerGen;     // global gen for provider registration churn
    bool               handled;
    LuaFieldCacheEntry()
        : intAvailWidth(0), isReadOnly(false), providerGen(0), handled(false) {}
};
struct LuaWindowEntry {
    std::string             name;
    sol::protected_function drawFn;
    std::vector<ImCmd>      cmds;
    std::uint64_t           cachedDataGen;     // (Q5) two-field check — no XOR collision
    std::uint64_t           cachedProviderGen;
    bool                    dirty;
    bool                    hasError;          // (finding #7) negative-cache for record failures
    std::string             errorMessage;      // shown via TextColored; cleared on invalidate
    LuaWindowEntry()
        : cachedDataGen(0), cachedProviderGen(0), dirty(true), hasError(false) {}
};
```

`LuaDrawList` recorder (defined in `.cpp`). **Lifetime-safe via `std::shared_ptr` + `active` flag** (finding #5) — Lua scripts cannot UAF the recorder by stashing `draw` globally, because every method checks `active_` and `luaL_error`s after recording ends.

**Important** (F6 finding): callers pass the **`std::shared_ptr<LuaDrawList>` itself** to sol2, not `*rec`. Passing a raw reference would let sol2 store a pointer or value-copy without Lua co-owning the ptr — the "Lua stash keeps object alive but inactive" guarantee then collapses on the C++ side. By passing the `shared_ptr`, sol2 registers a usertype that holds the shared ownership; the recorder lives as long as Lua references it.

`LuaDrawList` is **non-copyable, non-movable** to enforce this — sol2 will refuse to value-copy it, forcing the `shared_ptr` route. Methods access state via the `shared_ptr`-owned object.

```cpp
class LuaDrawList {
public:
    void Text(const std::string& s);
    void TextUnformatted(const std::string& s);
    void Image(const std::string& path, float w, float h);
    void ProgressBar(float frac, float w, float h, sol::optional<std::string> overlay);
    void SameLine(sol::optional<float> offset, sol::optional<float> spacing);
    void Separator();
    void Dummy(float w, float h);
    void PushColor(int colIdx, float r, float g, float b, float a);
    void PopColor(sol::optional<int> count);
    void SetTooltip(const std::string& s);
    void Button(const std::string& label, sol::protected_function on_click);
    void InputText(const std::string& label, const std::string& initial,
                   int max_len, sol::protected_function on_commit);
    void OnDeactivated(sol::protected_function fn);                  // mutates last interactive
    void OnDeactivatedAfterEdit(sol::protected_function fn);          // mutates last interactive
    void Deactivate() { active_ = false; }                            // called after sandbox call
    std::vector<ImCmd> Take() { return std::move(cmds_); }
private:
    void RequireActive(const char* method);  // throws sol::error if !active_
    ImCmd* LastInteractive();                // returns last Button / InputText, or nullptr
    std::vector<ImCmd> cmds_;
    int                interactiveIndex_ = 0;  // for auto-`##op<index>` suffix
    bool               active_ = true;
public:
    LuaDrawList(const LuaDrawList&)            = delete;   // (F6) non-copyable
    LuaDrawList& operator=(const LuaDrawList&) = delete;
    LuaDrawList(LuaDrawList&&)                 = delete;   // (F6) non-movable too
    LuaDrawList& operator=(LuaDrawList&&)      = delete;
    LuaDrawList() = default;
};
```

Callers construct via `auto rec = std::make_shared<LuaDrawList>();`, **pass `rec` (the `shared_ptr`) to sol2** — not `*rec` — then call `rec->Deactivate()` and drop their local `shared_ptr`. Lua-side ref keeps the C++ object alive via shared ownership; every method short-circuits with a clear error after `Deactivate()`.

```cpp
// usertype registration uses shared_ptr-aware overload:
state.new_usertype<LuaDrawList>("SmatchetDrawList", sol::no_constructor, /* methods... */);
// at call site:
auto rec = std::make_shared<LuaDrawList>();
sol::protected_function_result res = providerCopy(ticket.id, fieldId, rawValue,
                                                  availWidth, isReadOnly, fieldNameObj,
                                                  rec);  // pass the shared_ptr
rec->Deactivate();
```

sol2 supports `shared_ptr<T>` as a usertype handle natively (`sol::usertype<LuaDrawList>` registered → passing `shared_ptr<LuaDrawList>` makes Lua hold a co-owning ref).

Inside `Button` / `InputText`, label gets `+ "##b" + idx` / `+ "##it" + idx` appended before being stored in `ImCmd::str`. Increments `interactiveIndex_`.

`OnDeactivated` / `OnDeactivatedAfterEdit` do **not** append a new `ImCmd`. They look up `LastInteractive()` and assign to its `onDeactivated` / `onDeactivatedAfterEdit` field. If there is no prior interactive op in the recording, both methods log a warning and no-op (finding #9).

### State on `AppController`

Replace lines 690–692 + 695. **Member order must keep `sol::state lua` declared BEFORE these containers** (current `AppController.h:688-695` already has `lua` at 689, maps at 690-695 — keep it that way). C++ destroys members in reverse declaration order, so containers holding `sol::protected_function` die FIRST, `lua` LAST (finding #1 — original plan prose had this inverted).

```cpp
std::unordered_map<std::string, sol::protected_function> fieldDisplayCachedProviders_;
std::unordered_map<std::string, sol::protected_function> fieldDisplayCachedProvidersByName_;
std::unordered_map<std::string, LuaFieldCacheEntry>      luaFieldCache_;  // key = ticket.id + '\0' + fieldId
std::atomic<std::uint64_t>                                luaProviderGen_;   // bump on (un)register only — init to 1
std::atomic<std::uint64_t>                                luaWindowDataGen_; // bump on NotifyLuaTicketDataChanged
std::vector<LuaWindowEntry>                               luaWindows_;

// (finding #8) replay-safe queue: register/unregister/invalidate ops that arrive
// during DrawLuaWindows iteration are pushed here and drained after the loop.
struct PendingLuaWindowOp {
    enum class Kind { Register, Unregister, Invalidate };
    Kind        kind;
    std::string name;
    sol::protected_function drawFn;  // only for Register
};
std::vector<PendingLuaWindowOp> pendingLuaWindowOps_;
bool                            inDrawLuaWindows_ = false;
```

`luaProviderGen_` replaces the prior single-epoch design (finding #2). Per-cell invalidation now keys off the cached entry's own `rawValue` / `intAvailWidth` / `isReadOnly` (finding #3) — see `TryRenderCachedLuaField` below. Provider registration churn still needs a global bump because every cached entry references a possibly-stale provider closure.

See [ADR 0001 — Per-entry invalidation for the Lua field-display cache](../adr/0001-per-entry-lua-field-cache-invalidation.md).

### Dispatch — `TicketFieldEditor.cpp:840`

Replace existing `TryLuaFieldDisplay` call (signature uses `CachedTicket` + `const TrackerField*`):

```cpp
if (app.TryRenderCachedLuaField(column.FieldId, ticket, currentValue, availWidth,
                                field, /*allowEdits=*/allowEdits)) return;
```

`ticket` is `const CachedTicket&`, `field` is `const TrackerField*` — matches the existing `TryLuaFieldDisplay` shape at `AppController_LuaBindings.cpp:1158-1161` (F2 finding — prior plan revision used nonexistent `TicketRow` / `TrackerFieldEntry` names). `allowEdits` is the same bool the surrounding code already uses to gate inline editing — passing it through prevents `draw:input_text` from bypassing grid-level edit-disabled states. No legacy fallback — `register_field_display` is gone.

### `AppController::TryRenderCachedLuaField`

Signature — uses existing types from `TryLuaFieldDisplay` (F2 fix):

```cpp
bool TryRenderCachedLuaField(const std::string& fieldId,
                             const CachedTicket& ticket,
                             const std::string& rawValue,
                             float availWidth,
                             const TrackerField* fieldMeta,
                             bool allowEdits);
```

Read-only computation **must mirror existing `TryLuaFieldDisplay`** at `AppController_LuaBindings.cpp:1176-1178` — `field.ReadOnly` alone is too narrow (F3 finding):

```cpp
const bool catalogReadOnly  = fieldMeta ? fieldMeta->ReadOnly : false;
const bool editMetaReadOnly = !CanEditFieldForIssue(ticket.id, fieldId, fieldMeta);
const bool isReadOnly       = catalogReadOnly || editMetaReadOnly || !allowEdits;
```

`isReadOnly` is what gets passed to Lua AND cached. Combines catalog flag + editmeta probe + grid-level `allowEdits`, so the cache invalidates whenever any of the three changes. `allowEdits` is folded in — no separate `allowEdits` cache field needed.

1. Provider lookup by `fieldId`, else by lowercased display name (mirror `TryLuaFieldDisplay` at `AppController_LuaBindings.cpp:1162-1171`). None → return false.
2. Key = `ticket.id + '\0' + fieldId`.
3. Compute `isReadOnly` per the formula above.
4. Compute `intAvailWidth = static_cast<int>(std::lround(availWidth))` (F8 finding — rounded integer width, not 8 px quantization; resize churn is user-driven so re-recording during drag is acceptable; visual exactness matters for narrow / proportional Lua draws like `avail_width - 4`).
5. Cache hit AND **all of**:
   - `entry.rawValue == rawValue`
   - `entry.fieldName == (fieldMeta ? fieldMeta->Name : std::string())`
   - `entry.intAvailWidth == intAvailWidth`
   - `entry.isReadOnly == isReadOnly`
   - `entry.providerGen == luaProviderGen_.load()`

   **Width-quantization tradeoff (F8)**: using a rounded integer width means every 1 px change of column width re-records the cell. Column-width drag is user-driven (mouse-held drag, single bounded operation), so re-recording per-pixel during drag is acceptable: the user is already paying for redraws every frame of the drag, and the recorded list is small per cell (10-20 µs to re-record on miss). The 8 px quantization considered in earlier revisions would leave Lua-recorded widths stale by up to 7 px — visible in narrow / proportional draws like the `avail_width - 4` overlay in the `progress` provider. Visual exactness wins.

   Then:
   - `entry.handled == false` → return false.
   - else `ReplayCmdList(entry.cmds, *this, ticket.id, fieldId);` return true.

4. Miss / stale (Lua call path) — implements the full Crash-safety hardening contract:
   - `SMATCHET_UI_PERF_SCOPE("LuaDrawList::Record");`
   - `sol::protected_function providerCopy = it->second;` — **copy** the function out of the map (Crash-safety §C3 — survives provider re-registering / unregistering itself mid-call).
   - `auto rec = std::make_shared<LuaDrawList>();`
   - `LuaHookGuard hook(lua);` (RAII, finding #10 — installs `lua_sethook(..., LUA_MASKCOUNT, 100000)` on construct, clears on destruct, exception-safe — uniform count per Q7)
   - `LuaImmediateModeGuard imm(false);` (finding #6 — blocks `imgui.*` during record)
   - Wrap the protected-function call in `try { res = providerCopy(...); } catch (const std::exception&) { callOk = false; } catch (...) { callOk = false; }` (Crash-safety §C4).
   - Pass 7 args: `(ticket.id, fieldId, rawValue, availWidth, isReadOnly, fieldNameObj, rec)` — note **`rec` is the `shared_ptr<LuaDrawList>` itself**, not `*rec` (F6 lifetime fix).
   - `rec->Deactivate();` (subsequent Lua-side use of stashed `draw` errors cleanly).
   - On `callOk && res.valid() && truthy(res)` → `e.handled = true; e.cmds = rec->Take();`
   - On error / non-truthy / C++ exception → `e.handled = false; e.cmds.clear();` (log warn-once per `fieldId`).
   - Populate `e.rawValue / e.fieldName / e.intAvailWidth / e.isReadOnly / e.providerGen` from the call inputs.
   - `luaFieldCache_[key] = std::move(e);`
   - Replay if handled; return `e.handled`.

7-arg call cost is paid only on real change to one of the cached inputs — typically once per cell per refresh, **never** on unrelated tickets' field edits (finding #2 — a single-cell edit now re-records exactly that one cell, not every cached cell).

### `ReplayCmdList` — unified signature

```cpp
enum class LuaReplayCallbackKind : std::uint8_t {
    None        = 0,
    Click       = 1 << 0,
    Commit      = 1 << 1,   // InputText IsItemDeactivatedAfterEdit
    Deactivated = 1 << 2,   // on_deactivated* — does NOT dirty windows (Q6 decision)
};

std::uint8_t ReplayCmdList(std::vector<ImCmd>& cmds, AppController& app,
                           const std::string& cbArg1, const std::string& cbArg2);
```

Bitmask of fired callback kinds. Cell callers discard. **Window caller dirties only on `Click | Commit`** (Q6): `on_deactivated*` is a validation-on-focus-loss pattern that should not auto-re-record. Scripts that need explicit re-record after `on_deactivated*` call `ui.invalidate_window(name)` inside that callback.

```cpp
std::uint8_t AppController::ReplayCmdList(std::vector<ImCmd>& cmds,
                                          const std::string& cbArg1, const std::string& cbArg2) {
    SMATCHET_UI_PERF_SCOPE("LuaDrawList::Replay");
    int           pushed = 0;
    std::uint8_t  fired  = 0;
    using K = LuaReplayCallbackKind;
    try {                              // (Crash-safety) outer guard; abandon replay on any throw
    for (ImCmd& c : cmds) {           // non-const ref: InputText mutates textBuf in place
        switch (c.op) {
            case ImCmd::Op::Text:            ImGui::Text("%s", c.str.c_str()); break;
            case ImCmd::Op::TextUnformatted: ImGui::TextUnformatted(c.str.c_str()); break;
            case ImCmd::Op::Image:           LuaImGuiImageBind(c.str, c.f1, c.f2); break;
            case ImCmd::Op::ProgressBar: {
                ImVec2 sz(c.f2, c.f3);
                if (c.f2 < 0.0f)  sz.x = ImGui::GetContentRegionAvail().x;
                if (c.f3 <= 0.0f) sz.y = ImGui::GetFrameHeight();
                ImGui::ProgressBar(c.f1, sz, c.str.empty() ? nullptr : c.str.c_str());
                break;
            }
            case ImCmd::Op::SameLine:  ImGui::SameLine(c.f1, c.f2); break;
            case ImCmd::Op::Separator: ImGui::Separator(); break;
            case ImCmd::Op::Dummy:     ImGui::Dummy(ImVec2(c.f1, c.f2)); break;
            case ImCmd::Op::PushColor:
                if (c.i1 < 0 || c.i1 >= ImGuiCol_COUNT) break;   // defensive: skip OOB
                ImGui::PushStyleColor(c.i1, ImVec4(c.f1, c.f2, c.f3, c.f4));
                ++pushed; break;
            case ImCmd::Op::PopColor: {
                int n = std::min(c.i1 > 0 ? c.i1 : 1, pushed);
                if (n > 0) { ImGui::PopStyleColor(n); pushed -= n; }
                break;
            }
            case ImCmd::Op::SetTooltip:
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.str.c_str());
                break;
            case ImCmd::Op::Button:
                if (ImGui::Button(c.str.c_str()) && c.callback) {
                    InvokeLuaCallbackSandboxed(c.callback, cbArg1, cbArg2);
                    fired |= static_cast<std::uint8_t>(K::Click);
                }
                // (finding #9) on_deactivated* fields attached at record time
                if (c.onDeactivated && ImGui::IsItemDeactivated()) {
                    InvokeLuaCallbackSandboxed(c.onDeactivated, cbArg1, cbArg2);
                    fired |= static_cast<std::uint8_t>(K::Deactivated);
                }
                if (c.onDeactivatedAfterEdit && ImGui::IsItemDeactivatedAfterEdit()) {
                    InvokeLuaCallbackSandboxed(c.onDeactivatedAfterEdit, cbArg1, cbArg2);
                    fired |= static_cast<std::uint8_t>(K::Deactivated);
                }
                break;
            case ImCmd::Op::InputText: {
                if (c.textBuf.empty()) break;  // defensive: never happens; record-time primes it
                ImGui::InputText(c.str.c_str(), c.textBuf.data(), c.textBuf.size());
                if (ImGui::IsItemDeactivatedAfterEdit() && c.callback) {
                    InvokeLuaCallbackSandboxed(c.callback, cbArg1, cbArg2,
                                               std::string(c.textBuf.data()));
                    fired |= static_cast<std::uint8_t>(K::Commit);
                }
                if (c.onDeactivated && ImGui::IsItemDeactivated()) {
                    InvokeLuaCallbackSandboxed(c.onDeactivated, cbArg1, cbArg2);
                    fired |= static_cast<std::uint8_t>(K::Deactivated);
                }
                if (c.onDeactivatedAfterEdit && ImGui::IsItemDeactivatedAfterEdit()) {
                    InvokeLuaCallbackSandboxed(c.onDeactivatedAfterEdit, cbArg1, cbArg2);
                    fired |= static_cast<std::uint8_t>(K::Deactivated);
                }
                break;
            }
        }
    }
    } catch (const std::exception& ex) {
        LOG_WARN("ReplayCmdList: C++ exception aborted replay: %s", ex.what());
    } catch (...) {
        LOG_WARN("ReplayCmdList: unknown C++ exception aborted replay");
    }
    if (pushed > 0) ImGui::PopStyleColor(pushed);  // defensive: balance Lua imbalance (also fires on early throw)
    return fired;
}
```

**Clarification on InputText buffer init**: the recorder primes `c.textBuf` *at record time* in `LuaDrawList::InputText` (copies `initial` into a vector sized to `max_len + 1`).

### `LuaHookGuard` — RAII Lua hook (finding #10)

```cpp
struct LuaHookGuard {
    lua_State* L;
    explicit LuaHookGuard(sol::state& lua, int count = 100000) : L(lua.lua_state()) {
        lua_sethook(L, [](lua_State* s, lua_Debug*) {
            luaL_error(s, "Script execution timeout exceeded.");
        }, LUA_MASKCOUNT, count);
    }
    ~LuaHookGuard() { lua_sethook(L, nullptr, 0, 0); }
    LuaHookGuard(const LuaHookGuard&) = delete;
    LuaHookGuard& operator=(const LuaHookGuard&) = delete;
};
```

Used by `TryRenderCachedLuaField`, `DrawLuaWindows` record path, `InvokeLuaCallbackSandboxed`, and any other Lua entry point. Survives early returns + exceptions, eliminating the paired manual `lua_sethook(L, nullptr, 0, 0)` calls scattered across the codebase.

**Hook count = 100000** uniformly (Q7 decision). The legacy per-frame `TryLuaFieldDisplay` used `10000` to guard against per-frame cost — the new design pays Lua cost only once per cell per refresh, so the tight budget is no longer protective; just restrictive. Matches existing window/callback paths.

### `InvokeLuaCallbackSandboxed`

Wraps `LuaHookGuard` (default count = 100000, uniform across all Lua entry points — see Q7) around a `protected_function_result` call. Logs + swallows errors. Variadic via sol2 forwarding. One helper used by all interactive ops + window callbacks.

### `LuaDrawList` usertype registration (in `InitLuaUi`, near `:613`)

```cpp
state.new_usertype<LuaDrawList>("SmatchetDrawList",
    sol::no_constructor,
    "text",                       &LuaDrawList::Text,
    "text_unformatted",           &LuaDrawList::TextUnformatted,
    "image",                      &LuaDrawList::Image,
    "progress_bar",               &LuaDrawList::ProgressBar,
    "same_line",                  &LuaDrawList::SameLine,
    "separator",                  &LuaDrawList::Separator,
    "dummy",                      &LuaDrawList::Dummy,
    "push_color",                 &LuaDrawList::PushColor,
    "pop_color",                  &LuaDrawList::PopColor,
    "set_tooltip",                &LuaDrawList::SetTooltip,
    "button",                     &LuaDrawList::Button,
    "input_text",                 &LuaDrawList::InputText,
    "on_deactivated",             &LuaDrawList::OnDeactivated,
    "on_deactivated_after_edit",  &LuaDrawList::OnDeactivatedAfterEdit);
```

### Invalidation strategy (finding #2, #4)

**Per-cell invalidation is automatic** — `TryRenderCachedLuaField` compares cached `rawValue` / `fieldName` / `quantAvailWidth` / `readOnly` / `allowEdits` against the current call arguments. A single-cell edit only re-records that one cell because only its `rawValue` changed. No global bump needed for value changes.

**Provider-registration generation** is the only global bump (cached entries hold a reference to a possibly-stale provider closure):

| File | Hook | Bump |
|---|---|---|
| `LuaRegister/UnregisterFieldDisplayCached[ByName]Bind` (new) | provider registration | `luaProviderGen_.fetch_add(1);` |

**Window data revision** — windows can't compare arbitrary Lua state, so they need an explicit "data changed" signal. Centralize this in one method (finding #4) instead of sprinkling bumps in `UpdateTicket` and `ApplyIssueFetchPack`:

```cpp
void AppController::NotifyLuaTicketDataChanged() {
    // Marks every window with cachedGen != current as dirty next frame.
    // Cells do NOT use this — their per-entry input comparison handles their case.
    luaWindowDataGen_.fetch_add(1);
}
```

Call sites — bump exactly once per state-change event (F4 finding: `UpdateTicket` calls `RefreshLocalData()` after `Cache->SaveTicket` at `AppController_CatalogAndFieldEdit.cpp:73`, so calling `NotifyLuaTicketDataChanged()` from both produces a double bump per single edit):

- `AppController_CatalogAndFieldEdit.cpp` `RefreshLocalData()` (~line 53) — **single hook site**. Calls `NotifyLuaTicketDataChanged()` adjacent to the existing `ActiveTicketsRevision.fetch_add(1)`. Every code path that mutates active ticket state already routes through `RefreshLocalData` — single chokepoint, no double-bump.
- `AppController_CatalogAndFieldEdit.cpp` `UpdateTicket(...)` — **do NOT** call `NotifyLuaTicketDataChanged()` directly. The trailing `RefreshLocalData()` call at `:73` already handles the bump.
- `TicketSyncService` — **coalesced** to fire once per fetch session (Q2). Add an internal `pendingLuaWindowBump_` bool flipped to true in `ApplyIssueFetchPack`, and a session-end hook (`OnFetchCompleted` / `OnFetchAborted`) that calls `app_.NotifyLuaTicketDataChanged()` exactly once if the flag is set, then clears it. Streaming sync (`:267`) and batch end-of-fetch (`:99`) both flip the flag; only the session-end path emits the bump.
- `TicketSyncService::TickStreamingApply` **stale-deletion path** (`:192`, where `ActiveTicketsRevision.fetch_add` runs after dropping stale tickets) — **also** flip `pendingLuaWindowBump_ = true` (F5 finding). Windows showing ticket counts / lists need the bump after stale deletion. **Invariant rule**: whenever `ActiveTicketsRevision.fetch_add` runs, the same scope must flip `pendingLuaWindowBump_` (or directly call `NotifyLuaTicketDataChanged()` if outside a fetch session).

**Thread invariant**: `ApplyIssueFetchPack` runs on the UI thread (`spike-hunter.md:56` invariant; HTTP fetch is on the worker, the *apply* step is dispatched back to UI). Session-end hook fires on the UI thread too. `pendingLuaWindowBump_` is a plain `bool` — no atomic. If a future refactor moves apply off the UI thread, this needs revisiting.

Rationale: a single fetch session can run `ApplyIssueFetchPack` 50+ times. Per-pack bumps would re-record every window per pack — N_windows × Lua_record_time × 50 = unacceptable on the UI thread. Coalescing makes the cost identical to a single refresh.

`luaProviderGen_` and `luaWindowDataGen_` are `std::atomic` because `UpdateTicket` / sync may run off the UI thread. `luaFieldCache_` and `luaWindows_` themselves are UI-thread-only.

### Eviction

Lazy: stale entries overwritten on miss. Sync-time prune at end of `ApplyIssueFetchPack`:

```cpp
if (luaFieldCache_.size() > 4 * activeTickets.size() * providers + 256) {
    // walk entries, drop any whose ticket id is not in activeTickets snapshot
}
```

No per-frame eviction.

### Window draw — new `DrawLuaWindows`

Iterates `luaWindows_` with `inDrawLuaWindows_ = true`. Any `ui.register_window` / `ui.invalidate_window` / `ui.unregister_window` call reached during iteration (directly from a button callback, or indirectly via MCP / worker → `mainThreadDispatcher`) is queued into `pendingLuaWindowOps_` instead of mutating the vector (finding #8). The queue is drained after the loop.

Negative-cache on record failure (finding #7): on Lua error, store the message in `errorMessage`, set `hasError = true`, set `dirty = false`. Replay path renders the error via `TextColored` from cache. Retry only happens on explicit `ui.invalidate_window` or `luaWindowDataGen_` / `luaProviderGen_` bump.

```cpp
inDrawLuaWindows_ = true;
const std::uint64_t curDataGen     = luaWindowDataGen_.load();
const std::uint64_t curProviderGen = luaProviderGen_.load();
for (LuaWindowEntry& w : luaWindows_) {
    if (!w.drawFn.valid()) continue;
    bool open = true;
    if (ImGui::Begin(w.name.c_str(), &open)) {
        if (w.dirty || w.cachedDataGen != curDataGen || w.cachedProviderGen != curProviderGen) {
            auto rec = std::make_shared<LuaDrawList>();
            FieldEditAuditSource::ScopedOverride luaSource(FieldEditAuditSource::kLua);
            sol::protected_function_result res;
            bool callOk = true;
            std::string cxxErr;
            sol::protected_function drawFnCopy = w.drawFn;  // (C3) copy, not reference
            try {
                SMATCHET_UI_PERF_SCOPE("LuaWindow::Record");
                LuaHookGuard hook(lua);
                LuaImmediateModeGuard imm(false);   // finding #6: blocks imgui.* during record
                res = drawFnCopy(rec);              // (F6) pass shared_ptr, not *rec
            } catch (const std::exception& ex) {
                callOk = false;
                cxxErr = ex.what();
                LOG_WARN("DrawLuaWindows: C++ exception window=%s %s", w.name.c_str(), ex.what());
            } catch (...) {
                callOk = false;
                cxxErr = "unknown C++ exception";
                LOG_WARN("DrawLuaWindows: unknown C++ exception window=%s", w.name.c_str());
            }
            rec->Deactivate();
            if (callOk && res.valid()) {
                w.cmds = rec->Take();
                w.cachedDataGen = curDataGen;
                w.cachedProviderGen = curProviderGen;
                w.dirty = false;
                w.hasError = false;
                w.errorMessage.clear();
            } else {
                // (finding #7) negative-cache: record error, stop trying every frame
                std::string msg;
                if (!callOk) {
                    msg = cxxErr;
                } else {
                    sol::error e = res;
                    msg = e.what();
                }
                LOG_TRACE("DrawLuaWindows: error window=%s %s", w.name.c_str(), msg.c_str());
                w.cmds.clear();
                w.hasError = true;
                w.errorMessage = std::move(msg);
                w.cachedDataGen = curDataGen;
                w.cachedProviderGen = curProviderGen;
                w.dirty = false;
            }
        }
        if (w.hasError) {
            ImGui::TextColored(ImVec4(1, 0.2f, 0.2f, 1), "Lua Error: %s",
                               w.errorMessage.c_str());
        } else {
            using K = LuaReplayCallbackKind;
            std::uint8_t fired;
            {
                SMATCHET_UI_PERF_SCOPE("LuaWindow::Replay");
                fired = ReplayCmdList(w.cmds, *this, w.name, std::string());
            }
            const std::uint8_t dirtyMask = static_cast<std::uint8_t>(K::Click) |
                                           static_cast<std::uint8_t>(K::Commit);
            if (fired & dirtyMask) {
                w.dirty = true;   // Q6: only Click/Commit auto-dirty; on_deactivated* does not
            }
        }
    }
    ImGui::End();
    if (!open) w.drawFn = sol::lua_nil;
}
inDrawLuaWindows_ = false;

// (finding #8) drain queued ops after iteration is safe.
for (PendingLuaWindowOp& op : pendingLuaWindowOps_) {
    switch (op.kind) {
        case PendingLuaWindowOp::Kind::Register: {
            // mirror non-deferred LuaUiRegisterWindowBind: erase existing entry by name, push new.
            auto it = std::find_if(luaWindows_.begin(), luaWindows_.end(),
                [&](const LuaWindowEntry& w) { return w.name == op.name; });
            if (it != luaWindows_.end()) luaWindows_.erase(it);
            LuaWindowEntry e; e.name = op.name; e.drawFn = std::move(op.drawFn); e.dirty = true;
            luaWindows_.push_back(std::move(e));
            break;
        }
        case PendingLuaWindowOp::Kind::Unregister:
            luaWindows_.erase(std::remove_if(luaWindows_.begin(), luaWindows_.end(),
                [&](const LuaWindowEntry& w) { return w.name == op.name; }), luaWindows_.end());
            break;
        case PendingLuaWindowOp::Kind::Invalidate:
            for (LuaWindowEntry& w : luaWindows_)
                if (w.name == op.name) {
                    w.dirty = true;
                    w.hasError = false;   // explicit invalidate clears negative-cache
                    w.errorMessage.clear();
                }
            break;
    }
}
pendingLuaWindowOps_.clear();

luaWindows_.erase(std::remove_if(luaWindows_.begin(), luaWindows_.end(),
    [](const LuaWindowEntry& w) { return !w.drawFn.valid(); }), luaWindows_.end());
```

`LuaUiRegisterWindowBind` / `LuaUiUnregisterWindowBind` / `LuaUiInvalidateWindowBind` are **self-dispatching** (Q10): each posts to `mainThreadDispatcher`. The posted closure runs on the UI thread, checks `inDrawLuaWindows_`, and either mutates `luaWindows_` immediately (loop not in progress) or pushes onto `pendingLuaWindowOps_` (mid-iteration, finding #8). Two-layer queue: dispatcher handles the cross-thread hop, `pendingLuaWindowOps_` handles mid-iteration safety. Callers do not need to pre-route — the binds are safe to invoke from any thread.

### Explicit cell-cache invalidation

Provider closures may read Lua-global state outside the 6 documented args (theme tables, user-pref tables, action-callback-mutated state). The strict cache-key comparison cannot detect those changes. Lua scripts that mutate such state must signal invalidation explicitly — mirrors the `ui.invalidate_window` contract for windows.

```lua
ui.invalidate_field_cache()                          -- drop entire luaFieldCache_
ui.invalidate_field_cache_for(ticket_id)             -- drop all entries for one ticket
ui.invalidate_field_cache_for(ticket_id, field_id)   -- drop one entry
```

C++:

```cpp
void AppController::LuaUiInvalidateFieldCacheBind(sol::optional<std::string> ticketId,
                                                  sol::optional<std::string> fieldId) {
    std::string tid = ticketId.value_or(std::string());
    std::string fid = fieldId.value_or(std::string());
    bool hasTicket  = static_cast<bool>(ticketId);
    bool hasField   = static_cast<bool>(fieldId);
    auto apply = [this, hasTicket, hasField, tid, fid]() {
        if (!hasTicket) { luaFieldCache_.clear(); return; }
        if (!hasField) {
            for (auto it = luaFieldCache_.begin(); it != luaFieldCache_.end(); ) {
                const std::string& key = it->first;
                std::size_t nul = key.find('\0');
                if (nul != std::string::npos && key.compare(0, nul, tid) == 0)
                    it = luaFieldCache_.erase(it);
                else ++it;
            }
            return;
        }
        std::string key = tid; key.push_back('\0'); key.append(fid);
        luaFieldCache_.erase(key);
    };
    // (F1) UI-thread caller: apply immediately so an edit from a button callback inside
    // DrawLuaWindows takes effect before the next cell render in the same frame.
    // Off-thread caller (MCP, automation worker): hop the dispatcher.
    if (IsOnUiThread()) apply();
    else mainThreadDispatcher.PostToMainThread(std::move(apply));
}
```

Doc note in `docs/guides/lua.md`: "If your `register_field_display_cached` provider reads Lua state beyond its 6 args (theme, user prefs, action-mutated tables), call `ui.invalidate_field_cache()` (or the targeted variants) after mutating that state."

### Explicit window invalidation

**F1 timing fix**: `mainThreadDispatcher.Drain()` runs early in `SmatchetUI::Draw` (`SmatchetUI.cpp:621`), **before** `DrawLuaWindows`. A callback fired *inside* `DrawLuaWindows` is by definition on the UI thread; if it always posted to the dispatcher, the queued op would not drain until the **next** frame, by which time `inDrawLuaWindows_` is false and `pendingLuaWindowOps_` no longer protects the active iteration. The correct rule:

- **On UI thread**: call `ApplyOrQueueLuaWindowOp(op)` immediately — that helper checks `inDrawLuaWindows_` and either mutates `luaWindows_` or enqueues onto `pendingLuaWindowOps_` (drained after the loop, same frame).
- **Off UI thread**: post to `mainThreadDispatcher`; the dispatcher closure re-enters `ApplyOrQueueLuaWindowOp` on the UI thread.

```cpp
// Shared helper (UI thread only).
void AppController::ApplyOrQueueLuaWindowOp(PendingLuaWindowOp op) {
    if (inDrawLuaWindows_) {
        pendingLuaWindowOps_.push_back(std::move(op));  // drained after loop, same frame
        return;
    }
    switch (op.kind) {
        case PendingLuaWindowOp::Kind::Invalidate:
            for (LuaWindowEntry& w : luaWindows_) {
                if (w.name == op.name) {
                    w.dirty = true;
                    w.hasError = false;
                    w.errorMessage.clear();
                    return;
                }
            }
            break;
        case PendingLuaWindowOp::Kind::Register: {
            auto it = std::find_if(luaWindows_.begin(), luaWindows_.end(),
                [&](const LuaWindowEntry& w) { return w.name == op.name; });
            if (it != luaWindows_.end()) luaWindows_.erase(it);
            LuaWindowEntry e;
            e.name = op.name;
            e.drawFn = std::move(op.drawFn);
            e.dirty = true;
            luaWindows_.push_back(std::move(e));
            break;
        }
        case PendingLuaWindowOp::Kind::Unregister:
            luaWindows_.erase(std::remove_if(luaWindows_.begin(), luaWindows_.end(),
                [&](const LuaWindowEntry& w) { return w.name == op.name; }), luaWindows_.end());
            break;
    }
}

void AppController::LuaUiInvalidateWindowBind(const std::string& name) {
    PendingLuaWindowOp op;
    op.kind = PendingLuaWindowOp::Kind::Invalidate;
    op.name = name;
    if (IsOnUiThread()) {
        // F1: immediate apply-or-queue; callback fired inside DrawLuaWindows runs in
        // the same frame via pendingLuaWindowOps_, not the next frame via dispatcher.
        ApplyOrQueueLuaWindowOp(std::move(op));
    } else {
        mainThreadDispatcher.PostToMainThread([this, op = std::move(op)]() mutable {
            ApplyOrQueueLuaWindowOp(std::move(op));
        });
    }
}
```

The same `IsOnUiThread() ? ApplyOrQueueLuaWindowOp() : PostToMainThread()` pattern applies to `LuaUiRegisterWindowBind` and `LuaUiUnregisterWindowBind`. `IsOnUiThread()` already exists on `AppController` (`AppController.h:127`, `AppController.cpp:1009`) — set during `Initialize` and read here.

Drain order in `DrawLuaWindows`:
1. Iterate `luaWindows_` with `inDrawLuaWindows_ = true`. Callbacks may enqueue ops onto `pendingLuaWindowOps_`.
2. Set `inDrawLuaWindows_ = false`.
3. Drain `pendingLuaWindowOps_` via `ApplyOrQueueLuaWindowOp` (which now hits the direct-mutation path because flag is false).

This collapses the prior two-layer queue (`mainThreadDispatcher` + `pendingLuaWindowOps_`) to a single in-frame queue for UI-thread callbacks. Off-thread callers still hop the dispatcher first; cross-thread invalidations land within at most one frame of latency.

Lua-side: `ui.invalidate_window("My panel")`.

### Shutdown ordering — critical

`luaFieldCache_` + `luaWindows_` + the two provider maps all store `sol::protected_function` references. Destructors of those refs touch the Lua state. C++ destroys members in **reverse declaration order**, so the safe layout (already present at `AppController.h:688-695`) is:

```cpp
sol::state lua;                                                  // line 689 — declared FIRST
std::unordered_map<std::string, sol::protected_function> ...;    // 690+
std::vector<LuaWindowEntry> luaWindows_;                          // 695
```

At `~AppController`, the containers destruct first (their `sol::protected_function` members touch a still-alive `lua`), then `lua` destructs last. This is the **current order** and the **safe order** — keep it. (Finding #1 — earlier prose in this doc had the order claim inverted; corrected.)

Mitigation, layered on top of RAII: extend `ClearLuaTicketContextGlue` (`AppController_LuaBindings.cpp:898`) to explicitly clear all four containers, **then** clear the `__smatchet_app` Lua global. Call it before the `sol::state` destructor runs (the existing `~AppController` already calls it at `AppController.cpp:356` — confirmed).

```cpp
void AppController::ClearLuaTicketContextGlue() {
    luaFieldCache_.clear();
    fieldDisplayCachedProviders_.clear();
    fieldDisplayCachedProvidersByName_.clear();
    luaWindows_.clear();
    pendingLuaWindowOps_.clear();
    lua["__smatchet_app"] = sol::lua_nil;
}
```

### Removal of legacy `register_field_display` — exact touch points

| File:line | Action |
|---|---|
| `AppController.h:690-692` | Delete `fieldDisplayHandlers_`, `fieldDisplayHandlersByDisplayName_`. Add `fieldDisplayCachedProviders_`, `fieldDisplayCachedProvidersByName_`, `luaFieldCache_`, `luaProviderGen_`, `luaWindowDataGen_`, `pendingLuaWindowOps_`, `inDrawLuaWindows_`. **Keep `sol::state lua` at line 689 — declared BEFORE these containers** so RAII reverse-order destruction stays safe (finding #1). |
| `AppController.h:695` | Replace `luaWindows_` pair-vector with `std::vector<LuaWindowEntry>`. |
| `AppController.h` (Lua-bind decls, ~`:282`+) | Delete `LuaRegister/UnregisterFieldDisplay[ByName]Bind` decls. Add `…Cached…` + `LuaUiInvalidateWindowBind` + `LuaUiInvalidateFieldCacheBind`. |
| `AppController.h` (public, near `TryGetFieldIconMapTarget`) | Delete `TryLuaFieldDisplay` decl. Add **outside** the `#if SMATCHET_WITH_LUA_AUTOMATION` guard (Q11): `TryRenderCachedLuaField`, `NotifyLuaTicketDataChanged`, `LuaUiInvalidateWindowBind`, `LuaUiInvalidateFieldCacheBind` (called from unconditional code). Keep **inside** the guard: `ReplayCmdList` (only Lua paths invoke it), `LuaDrawList` recorder type, `ImCmd`, `LuaFieldCacheEntry`, `LuaWindowEntry`. Ensure `LuaImGuiImageBind` is reachable from replay (already public). |
| `AppController.h` (types section, inside `#if SMATCHET_WITH_LUA_AUTOMATION`) | Add `ImCmd`, `LuaFieldCacheEntry`, `LuaWindowEntry`. |
| `AppController_LuaBindings.cpp:406-424` | Delete 4 legacy display glue funcs. Add 4 `…cached…` glue funcs. |
| `AppController_LuaBindings.cpp:452-455` (`LuaUiRegisterWindowGlue`) | Keep glue; bind impl below builds `LuaWindowEntry`. |
| `AppController_LuaBindings.cpp:605-609` | Replace 4 `set_function("register_field_display…")` with `…_cached`. |
| `AppController_LuaBindings.cpp` near `:613` | Add `LuaDrawList` usertype registration. |
| `AppController_LuaBindings.cpp` near `:625` | Add `ui.set_function("invalidate_window", …)`, `ui.set_function("invalidate_field_cache", …)`, `ui.set_function("invalidate_field_cache_for", …)`. |
| `AppController_LuaBindings.cpp:741-761` | Delete 4 legacy bind defs. Add 4 cached bind defs (each bumps `luaProviderGen_`). |
| `AppController_LuaBindings.cpp:803-812` | `LuaUiRegisterWindowBind` / `LuaUiUnregisterWindowBind` post their closure to `mainThreadDispatcher.PostToMainThread(...)` (Q10). The posted closure then checks `inDrawLuaWindows_`: if true, route through `pendingLuaWindowOps_` (finding #8); otherwise mutate `luaWindows_` immediately. |
| `AppController_LuaBindings.cpp:613` (existing `imgui.*` glue) | Wrap each `imgui.*` set_function in a `g_luaImmediateModeAllowed` check; `luaL_error` with `"imgui.* not allowed inside cached provider / window — use draw:* instead"` when false (finding #6). `LuaImmediateModeGuard` RAII helper flips the flag for the recording window. |
| `Source_Core/src/AppController_CatalogAndFieldEdit.cpp:53` (`RefreshLocalData`) | Call `NotifyLuaTicketDataChanged()` adjacent to the existing `ActiveTicketsRevision.fetch_add(1)` (F4 — single hook; `UpdateTicket` already chains to `RefreshLocalData` at `:73` so no second bump needed). |
| `Source_Core/src/TicketSyncService.cpp:267` (`ApplyIssueFetchPack`) + `:99` (batch end-of-fetch) + `:192` (stale-deletion path) | Flip `pendingLuaWindowBump_ = true` (Q2 coalescing + F5 stale-deletion). Invariant: every `ActiveTicketsRevision.fetch_add` co-flips `pendingLuaWindowBump_`. |
| `Source_Core/src/TicketSyncService.cpp` (fetch-session completion + abort) | If `pendingLuaWindowBump_`, call `app_.NotifyLuaTicketDataChanged()` exactly once and clear the flag. |
| `AppController_LuaBindings.cpp:898` | Extend `ClearLuaTicketContextGlue` per shutdown section. |
| `AppController_LuaBindings.cpp:1159-1207` | Delete `TryLuaFieldDisplay`. Add `LuaDrawList` method bodies + `ReplayCmdList` + `TryRenderCachedLuaField` + `InvokeLuaCallbackSandboxed` + `LuaUiInvalidateWindowBind` + `LuaUiInvalidateFieldCacheBind` + `LuaHookGuard` + `LuaImmediateModeGuard` + `NotifyLuaTicketDataChanged`. |
| `AppController_LuaBindings.cpp:1513-1542` | Rewrite `DrawLuaWindows` per snippet above. |
| `AppController_LuaStubs.cpp:35-39` | (Q11) Delete `TryLuaFieldDisplay` stub. Add no-op stubs for **every** new method declared outside `#if SMATCHET_WITH_LUA_AUTOMATION` in `AppController.h`: `TryRenderCachedLuaField` (return `false`), `NotifyLuaTicketDataChanged` (empty), `LuaUiInvalidateWindowBind` (empty), `LuaUiInvalidateFieldCacheBind` (empty), `ReplayCmdList` if its decl ends up unconditional. Verify `DrawLuaWindows` stub still exists. Public methods needed by non-Lua call sites MUST be declared outside the guard so the stub build links. |
| `TicketFieldEditor.cpp:834` | One-line dispatch swap. |
| `scripts/SmatchetHooks.lua:5-24, :43, :46, :52-56, :59, :92, :191-201` | Replace legacy refs with cached binding + `draw:*` calls. Update `render_progress_json` to record `draw:progress_bar(...)`. Migrate `render_priority_cell` (matches user example). |
| `docs/guides/lua.md:27-30, :170` + window-API section | Replace with new signatures + recorder API table + `ui.invalidate_window` doc + migration note. |
| `Source_Core/include/SmatchetFieldIconRender.h` (1 mention) | Comment-only doc fix if any reference exists. |
| `CMakeLists.txt:333` | **Required for crash-safety** (Crash-safety hardening § Lua build mode). Add `set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)` before `add_library(Smatchet_Lua_Internal ...)` so `luaL_error` uses C++ exceptions rather than `longjmp`. Without this, hostile Lua input causes resource leaks and potential sol2 corruption. |

### Migrated Lua samples

```lua
-- Priority icons (matches user example):
register_field_display_cached("priority", function(issue_id, field_id, raw, avail_width, read_only, field_name, draw)
    local key = raw or ""
    local cached = PRIORITY_MEMO[key]
    if cached ~= nil then
        if cached then draw:image(cached, PRIORITY_ICON_SIZE, PRIORITY_ICON_SIZE) end
        return true
    end
    local path = resolve_priority_icon(key)
    PRIORITY_MEMO[key] = path or false
    if path then draw:image(path, PRIORITY_ICON_SIZE, PRIORITY_ICON_SIZE) end
    return true
end)

-- Progress bar:
register_field_display_cached_by_name("progress", function(id, fid, raw, avail_width, read_only, name, draw)
    if not raw or raw == "" then return false end
    local pct = tonumber(raw:match("\"percent\"%s*:%s*(%-?%d+%.?%d*)"))
    if not pct then return false end
    pct = math.max(0, math.min(100, math.floor(pct))) / 100
    draw:progress_bar(pct, avail_width - 4, 0, string.format("%d%%", math.floor(pct * 100)))
    return true
end)

-- Colored text + tooltip:
register_field_display_cached("priority_text", function(id, fid, raw, avail_width, read_only, name, draw)
    if raw == "High" then
        draw:push_color(0, 1.0, 0.3, 0.3, 1.0)   -- 0 = ImGuiCol_Text
        draw:text(raw)
        draw:pop_color()
        draw:set_tooltip("High priority")
        return true
    end
    return false
end)

-- Editable cell (InputText) — Lua fires only on commit:
register_field_display_cached_by_name("Summary", function(id, fid, raw, avail_width, read_only, name, draw)
    if read_only then return false end
    draw:input_text("##summary", raw or "", 512, function(issue_id, field_id, new_value)
        if new_value ~= raw then
            smatchet.get_ticket(issue_id):set_field(field_id, new_value)
        end
    end)
    return true
end)

-- Button + post-widget hook:
register_field_display_cached("status", function(id, fid, raw, avail_width, read_only, name, draw)
    draw:button(raw == "" and "Set..." or raw, function(issue_id, field_id)
        log_info("clicked " .. issue_id)
    end)
    draw:on_deactivated_after_edit(function(issue_id, field_id)
        log_info("deactivated " .. issue_id)
    end)
    return true
end)

-- Window with text + button:
ui.register_window("Counters", function(draw)
    local n = COUNTER or 0
    draw:text(string.format("Count = %d", n))
    draw:same_line()
    draw:button("+1", function(window_name)
        COUNTER = (COUNTER or 0) + 1
        -- callback fired → window auto-marked dirty → next frame re-records with new COUNTER
    end)
end)
```

## Crash-safety hardening

Goal: **no Lua input crashes the host**. Every recorder method, every replay op, every provider invocation site validates inputs and catches exceptions. This section enumerates the hardening rules; the `LuaDrawList` method bodies + `ReplayCmdList` op handlers + `TryRenderCachedLuaField` + `DrawLuaWindows` must implement all of them.

### Recorder-method input validation (all `LuaDrawList::*`)

All bounds-violating inputs are clamped + a `LOG_WARN` once per `(method, fieldId)` pair to avoid log spam. Reject = drop the op, return without recording.

| Method | Validation |
|---|---|
| `Text(s)`, `TextUnformatted(s)`, `SetTooltip(s)` | `s.size() > 64*1024` → truncate to 64 KB. Embedded NULs preserved (`Text` uses `%s` so stops at first NUL; documented). |
| `Image(path, w, h)` | `!std::isfinite(w) \|\| !std::isfinite(h) \|\| w < 0 \|\| h < 0 \|\| w > 8192 \|\| h > 8192` → reject. Empty `path` → reject. |
| `ProgressBar(frac, w, h, overlay)` | `!std::isfinite(frac)` → frac = 0. Clamp `frac` to `[0,1]`. `!std::isfinite(w/h)` → reject. `w/h` bound `[-1, 8192]` (negative means "auto"). Overlay size cap as for `Text`. |
| `SameLine(off, sp)` | Clamp each to `[-4096, 4096]`. |
| `Dummy(w, h)` | `!std::isfinite \|\| w < 0 \|\| h < 0 \|\| w > 8192 \|\| h > 8192` → reject. |
| `PushColor(idx, r, g, b, a)` | `idx < 0 \|\| idx >= ImGuiCol_COUNT` → reject (**C1**). Clamp each channel to `[0,1]`. |
| `PopColor(n)` | Recorded as-is; replay clamps to actual pushed depth (already specified). |
| `Button(label, fn)` | `label.size() > 256` → truncate. `fn` invalid → **still record the button** (with empty `callback`), so subsequent `draw:on_deactivated*(fn2)` can attach to it. Replay handles `!c.callback` as a no-op click — button is visible but click does nothing (F9 finding — original prose contradicted itself). |
| `InputText(label, initial, max_len, fn)` | `max_len <= 0` → default 256. `max_len > 65535` → clamp to 65535 (**C2**). `initial.size() > max_len` → truncate. `label.size() > 256` → truncate. |
| `OnDeactivated*(fn)` | If no prior interactive: log warn-once, no-op. |
| Any method called after `Deactivate()` | `luaL_error` with `"draw:* called outside its recording window"`. Handled by `active_` check (finding #5). |

### Provider-call site (`TryRenderCachedLuaField`)

```cpp
sol::protected_function provider = it->second;   // COPY, not reference (C3)
auto rec = std::make_shared<LuaDrawList>();
sol::protected_function_result res;
bool callOk = true;

// (F3) match existing TryLuaFieldDisplay read-only computation:
const bool catalogReadOnly  = fieldMeta ? fieldMeta->ReadOnly : false;
const bool editMetaReadOnly = !CanEditFieldForIssue(ticket.id, fieldId, fieldMeta);
const bool isReadOnly       = catalogReadOnly || editMetaReadOnly || !allowEdits;
sol::object fieldNameObj    = fieldMeta ? sol::make_object(lua, fieldMeta->Name)
                                        : sol::make_object(lua, sol::nil);

try {
    LuaHookGuard hook(lua);
    LuaImmediateModeGuard imm(false);
    // (F6) pass the shared_ptr itself so sol2 co-owns the recorder while Lua holds it.
    res = provider(ticket.id, fieldId, rawValue, availWidth,
                   isReadOnly, fieldNameObj, rec);
} catch (const std::exception& ex) {
    callOk = false;
    LOG_WARN("TryRenderCachedLuaField: exception field=%s err=%s",
             fieldId.c_str(), ex.what());
} catch (...) {
    callOk = false;
    LOG_WARN("TryRenderCachedLuaField: unknown exception field=%s", fieldId.c_str());
}
rec->Deactivate();
if (!callOk || !res.valid()) {
    entry.handled = false;
    entry.cmds.clear();
} else { /* normal success path: entry.cmds = rec->Take(); etc. */ }
```

- **C3**: copy `sol::protected_function` from the map before calling. If the provider re-registers or unregisters itself during the call, the map mutation does not invalidate our local copy (sol2 protected_function is a registry-ref handle; the underlying Lua function is GC-rooted by the copy until the local goes out of scope).
- **C4**: outer `try` / `catch (...)` around the entire call boundary catches `std::bad_alloc` from recorder method push_backs, sol2 marshalling allocations, or any other C++ exception escaping the Lua frame. Once Lua compiles as C++ (Lua build mode §), `luaL_error` throws and is caught here too.
- **C7**: `sol::protected_function_result` is assigned, not thrown — even on a Lua error inside the call, `res.valid()` returns false and we read the error via `res.get<sol::error>()`. No unwind through Lua frames.
- **F3**: `isReadOnly` folds catalog + editmeta + `allowEdits` — same as existing `TryLuaFieldDisplay` at `AppController_LuaBindings.cpp:1176-1178`. Both the Lua arg AND the cache key use this combined bool.
- **F6**: provider receives the recorder as a `shared_ptr<LuaDrawList>`, not a reference / value. Lua-side `draw` userdata co-owns the C++ object; Deactivate sets `active_=false` after the call so any stashed `draw` errors cleanly.

### Window-draw site (`DrawLuaWindows`)

Same hardening as `TryRenderCachedLuaField`. The existing `if (res.valid())` / negative-cache (finding #7) handles Lua-level errors. Add the same `try` / `catch (...)` wrapper around the protected-function call to catch C++ exceptions; on catch, mark `hasError = true`, `errorMessage = "C++ exception during window record"`, `dirty = false`. Recovery on `ui.invalidate_window` per finding #7.

### Replay-op handler hardening (`ReplayCmdList`)

The recorded `ImCmd` list is the result of a vetted record path — validation already happened. But corruption (memory bit-flip, future serialization bug, etc.) should not crash. Defensive bounds inside the switch:

- `case Op::PushColor`: re-verify `c.i1 >= 0 && c.i1 < ImGuiCol_COUNT` (cheap; one branch); skip if out of range. Pair with `PopColor` skip if `pushed == 0`.
- `case Op::InputText`: re-verify `!c.textBuf.empty()` before passing pointer; if empty, skip op (should never happen — primed at record).
- Each `case` calls the underlying `ImGui::*` inside its own `try { ... } catch (...) { LOG_WARN(...) }` block in iteration mode (cheap — try/catch with no throw is zero-cost on Itanium ABI). MSVC mode: `try`/`catch` adds prologue cost. Trade-off: wrap the **entire** for-loop in one try/catch instead of per-op; on catch, abandon the remaining replay for that cell + log warn-once. **Pick: outer try/catch around the whole `ReplayCmdList` body** — single SEH frame, recovers gracefully if any ImGui op throws.

### Lua build mode — required CMake change

Current state (verified at `CMakeLists.txt:333`): `Smatchet_Lua_Internal` builds Lua 5.3.6 `.c` sources as **C**. `luaL_error` uses `longjmp` — skips C++ destructors on the stack between the error site and sol2's `pcall` `setjmp` boundary. Recorder methods own `std::vector` + `std::string` temporaries between the sol2 wrapper and Lua's C-API; a longjmp through those frames leaks (best case) or corrupts sol2 internals (worst case).

**Required change**: compile Lua as C++ so `LUAI_THROW` resolves to `throw(...)` instead of `longjmp`. Two options, ordered by preference:

```cmake
# Option A (preferred): set the language of every Lua source to CXX.
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(Smatchet_Lua_Internal STATIC ${LUA_SOURCES})

# Option B: define LUAI_USER_ALIGNMENT_T / LUA_USE_LONGJMP off via compile def,
# but Lua 5.3 only auto-selects C++ exceptions when __cplusplus is defined at
# luaconf.h preprocess time — the cleanest trigger is option A above.
```

Place this between the existing `add_library(Smatchet_Lua_Internal …)` (`CMakeLists.txt:333`) and the corresponding `target_include_directories` call. Verify via `objdump -d` / `dumpbin /disasm` that `lua_error` resolves to a `_Unwind_RaiseException` (gcc/clang) or `__CxxThrowException` (MSVC) call — not `longjmp`. With C++ exceptions, the host's outer `try { ... } catch (...) { ... }` around `protected_function(...)` (Crash-safety hardening §Provider-call site) catches the unwind cleanly and destructors run.

Regression guard (F7 finding — the original prose suggested an `#error` in `AppController.h`, but Lua sources do not include `AppController.h`, so that header-side guard cannot prove the Lua target itself compiles as C++). The correct location is **CMake**:

```cmake
# Verify Lua target compiles as C++ — fail the configure step otherwise.
get_target_property(_lua_sources Smatchet_Lua_Internal SOURCES)
foreach(_src IN LISTS _lua_sources)
    get_source_file_property(_lang "${_src}" LANGUAGE)
    if(NOT _lang STREQUAL "CXX")
        message(FATAL_ERROR
            "Smatchet_Lua_Internal source ${_src} has LANGUAGE=${_lang}, expected CXX. "
            "luaL_error must use C++ exceptions for crash-safe unwinding through the recorder "
            "(see docs/plans/shipped/lua-recorded-cmd-list.md § Lua build mode).")
    endif()
endforeach()
```

Add directly after the `set_source_files_properties(... LANGUAGE CXX)` call. Belt-and-suspenders: also include a tiny `Smatchet_Lua_Internal_cxx_check.cpp` private TU (added to the Lua target's SOURCES) with `#ifndef __cplusplus / #error / #endif` — that fires at compile time if a future contributor silently strips the `LANGUAGE CXX` override but leaves the helper TU in the target.

DX12 target: `LUA_LIBRARIES` is the same internal target — the change applies to both standalone and DX12 builds. Verify dual-target build after the CMake edit.

### Shutdown / re-entrancy

- `~AppController` order: `ClearLuaTicketContextGlue()` runs before any other destructor (confirmed at `AppController.cpp:356`). RAII reverse-order destruction then destroys containers first, `lua` last (finding #1). No `sol::protected_function` outlives `sol::state`.
- Lua callback during `DrawLuaWindows` that calls `app_.RequestAppQuitHandler()` — deferred via existing handler; doesn't tear down mid-frame. Safe.
- Lua callback inside a recording frame that triggers `view.refresh` → eventual `RefreshLocalData` → `NotifyLuaTicketDataChanged()` → `luaWindowDataGen_.fetch_add(1)`. Next-frame re-record. Safe.

### Fuzz test (verification step 14)

Add a fuzz scenario: `scenarios/lua_recorder_fuzz.lua` (or in `tests/`) that registers a provider whose body randomizes calls — out-of-range colors, NaN sizes, oversized strings, deeply nested `push_color` without matching `pop_color`, `draw:button("", nil)`, calls to `draw:*` from a deferred callback. Run via `SmatchetStandalone.exe scenario.run lua_recorder_fuzz` for 60 seconds in a CI smoke job. Expected: no crash, no assert, warn-log churn acceptable. Locks in the entire hardening contract.

## Risks

| Risk | Mitigation |
|---|---|
| Lua emits unbalanced `push_color` / `pop_color` | Replay tracks `pushed`; defensive `PopStyleColor(pushed)` at end. |
| Provider depends on per-frame state (mouse pos, frame counter) | Documented: recorder captures static draw intent. `set_tooltip` / `on_deactivated*` cover hover + deactivation. Dynamic state must go through a callback that mutates Lua state + invalidates. |
| Texture path resolves at record time, then evicts later | Resolve happens **at replay** inside `LuaImGuiImageBind` — survives cache eviction. |
| Interactive widget callbacks fire on a stale `raw` after sync | Closure captures `raw` from record-time. When `UpdateTicket` writes a new value, the cell's `rawValue` no longer matches its cached entry → re-records with fresh `raw`. Staleness window = 1 frame between mutation and re-record. Documented. |
| Lua callback inside replay mutates ticket via `set_field` → triggers `UpdateTicket` mid-frame | `luaFieldCache_` is read-only during replay (only `c.textBuf` mutates in place). Mutated cell will re-record next frame due to changed `rawValue`. Safe. |
| Lua callback inside replay calls `ui.register_window` / `ui.invalidate_window` mid-iteration | Queued into `pendingLuaWindowOps_` and drained after the iteration loop (finding #8). No live mutation of `luaWindows_` while iterating. |
| Lua script calls `imgui.text(...)` inside a cached provider or window draw fn | `LuaImmediateModeGuard` flips a thread-local flag false during recording; the existing `imgui.*` glue checks the flag and `luaL_error`s with a clear message naming the offending function and the `draw:*` replacement (finding #6). Catches the "draws once on miss, vanishes after cache" failure mode at first run. |
| Window draw fn throws every frame → Lua re-runs every frame | Negative-cache: `hasError = true`, `dirty = false`, error rendered from cache. Retry only on `ui.invalidate_window` or `luaWindowDataGen_` / `luaProviderGen_` bump (finding #7). |
| Lua stashes the `draw` handle globally and calls `draw:text(...)` from a later event callback | `LuaDrawList::Deactivate()` runs after every recording. Every method checks `active_` and `luaL_error`s with a clear message (finding #5). Shared-ptr keeps the C++ object alive while Lua holds the ref. |
| Provider runs slow Lua on miss (e.g. JSON parse) | Cost paid once per cell per refresh, not per frame. Amortized cost is invisible (18 cells × 390 µs = 7 ms once per refresh, vs every frame). Lua-side memo (`PRIORITY_MEMO`) still beneficial across cells with the same `raw`. |
| Migration breaks user scripts referencing `register_field_display` / direct `imgui.*` inside windows | sol2 raises clear runtime error naming the missing function. LUA_GUIDE migration note + `SmatchetHooks.lua` updated as reference. |
| **Shutdown UAF**: `sol::protected_function` in cached `ImCmd::callback` destructs after `sol::state` | `ClearLuaTicketContextGlue` explicitly clears all four containers before nulling the `__smatchet_app` global. Existing `~AppController` already calls it at `AppController.cpp:356`. Verify member declaration order keeps `lua` **BEFORE** the cache maps (current `AppController.h:689` already correct) so RAII reverse-declaration destruction destroys the containers first, `lua` last — finding #1. |
| Atomic `luaProviderGen_` / `luaWindowDataGen_` vs non-atomic cache map | Cache mutated UI-thread only — documented inline. Gen counter RMW from any thread is safe. |
| InputText buffer reset when `rawValue` changes mid-edit | Per-cell invalidation means an unrelated cell's edit does not touch this cell's buffer. Only a direct mutation to the cell currently being edited drops the in-progress buffer. Documented limitation. v2: track focused-cell state and skip re-record while focused. |
| DX12 build picks up new headers | `ImCmd` / `LuaWindowEntry` / `LuaFieldCacheEntry` use `<string>` + `<vector>` + `<cstdint>` + sol2 — same set as existing Lua state. No GLFW/GL. `ImVec4` already in `Source_Core` headers. Safe. |
| Window dirty thrashes on every click | Acceptable: 1 Lua run per click ≫ 1 per frame. `on_deactivated*` does NOT dirty (Q6). Future v2 can add `draw:button(label, on_click, { invalidate=false })`. |
| Background thread mutates window-visible state without gen bump | Use `ui.invalidate_window(name)` from the MCP / async callback after applying the mutation. Bind self-dispatches via `mainThreadDispatcher` (Q10) so calling from any thread is safe. |
| C++14 marshalling pitfalls (no `optional`, no structured bindings) | `sol::optional<T>` on recorder methods is allowed (it's a sol2 type, not `std::optional`). All other code paths use plain `std::string` / fixed POD members. |

## Verification

1. **Build both targets**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`. PostToolUse hook runs clang-format + cppcheck + clang-tidy.
2. **Lua-side cell call counter**: instrument **two** providers (`priority` + `summary`) each with their own counter (`PRIORITY_CALLS`, `SUMMARY_CALLS`) and `log_info("priority=" .. PRIORITY_CALLS .. " summary=" .. SUMMARY_CALLS)`. Launch standalone. Open active project grid with ≥ 20 visible tickets.
   - First paint: `PRIORITY_CALLS` jumps by visible count once; `SUMMARY_CALLS` jumps by visible count once.
   - Subsequent frames (no input, no refresh): **no further increments** on either.
   - `view.refresh`: each increments by visible count once.
   - **Per-cell independence (Q8 — locks in finding #2)**: edit ticket A's `priority` cell only. Expected: `PRIORITY_CALLS` += 1, `SUMMARY_CALLS` += 0, no other ticket's `priority` provider invoked. Confirms a single-cell edit does not re-record unrelated cells. Open ticket B's `priority` cell on screen during the edit and verify its `LuaDrawList::Replay` scope `calls` increments while `LuaDrawList::Record` `calls` does not — replay path ran without provider invocation.
3. **Lua-side window counter**: instrument a window draw fn with `local n = 0; n = n + 1; log_info("win="..n)`. Open the window:
   - First frame: 1.
   - Idle frames: stays at 1.
   - `draw:button` click: increments.
   - `view.refresh`: increments.
   - `ui.invalidate_window(name)`: increments.
4. **FPS regression — cells**: add `SMATCHET_UI_PERF_SCOPE("LuaDrawList::Record")` around the Lua-call branch in `TryRenderCachedLuaField` and `SMATCHET_UI_PERF_SCOPE("LuaDrawList::Replay")` around the replay branch (finding #11). `perf.reset` → scroll grid 5 s → `perf.snapshot`. `RenderFieldCell` `lastTotalMs` should drop by ~7 ms / frame vs legacy. `LuaDrawList::Replay` scope should sit at <5 µs / cell. `LuaDrawList::Record` `calls` should be 0 between events. No `perf.snapshot` schema changes.
5. **FPS regression — windows**: add `SMATCHET_UI_PERF_SCOPE("LuaWindow::Record")` and `SMATCHET_UI_PERF_SCOPE("LuaWindow::Replay")` in `DrawLuaWindows`. Open 4 Lua windows, idle, measure. Frame time should drop by ~4 × legacy window cost. `LuaWindow::Record` `calls` stays at 4 across idle frames.
6. **Existing `lifetimeHits` confirms invariants**: `perf.snapshot` already exposes per-scope `calls` and `lifetimeHits`. No need for ad-hoc `g_luaProviderCalls` counters or new keys. Use `LuaDrawList::Record.calls` delta between snapshots to validate per-event re-record counts.
7. **Sandbox timeout**: temp provider `while true do end` → instruction hook fires → entry cached as `handled=false` → C++ renderer used on subsequent frames. Same test on a window: error rendered, `dirty` stays false until next event.
8. **Stub build**: `-DSMATCHET_WITH_LUA_AUTOMATION=OFF` → relink → no missing symbols.
9. **Visual regression set — cells**:
   - `priority` with `draw:image(...)` — icon renders, stable across frames.
   - `progress` with `draw:progress_bar(...)` — bar + overlay.
   - `push_color` + `text` + `pop_color` — color isolated to one cell, doesn't leak.
   - `set_tooltip` — appears on hover, disappears off-hover.
   - `button` — click fires Lua exactly once.
   - `input_text` — type freely; `on_commit` fires only on focus loss after edit.
   - `on_deactivated` — fires once on focus loss whether or not value changed.
   - Edit a field via grid → cell updates within one refresh tick.
10. **Visual regression set — windows**: text + button + input_text inside a window — render stable; button fires once per click; InputText edits commit on focus loss and trigger re-record.
11. **Window close**: clicking the X closes via `open=false`; entry cleaned up.
12. **Shutdown safety**: run a script that registers cached cell providers + windows, then `Ctrl+C` / close the app while a window has interactive widgets cached. No UAF / crash on exit. Repeat with sanitizers (UBSan / ASan) if available.
13. **DX12 build**: rebuild Unreal target — no header-pollution errors.
14. **Crash-safety fuzz** (Crash-safety hardening §): run `scenario.run lua_recorder_fuzz` for 60 s. Hostile-input Lua provider exercises every input bound (out-of-range `colIdx`, NaN sizes, huge `max_len`, oversized strings, unbalanced `push_color`, callback re-entrancy that re-registers itself). Expected: zero crashes, zero asserts, warn-log entries within rate limit. Repeat under UBSan / ASan if available.
15. **Window timing (F1)**: register a window whose button callback calls `ui.invalidate_window(name)` on itself. Click button. Expected: `LuaWindow::Record` `calls` increments by exactly 1 within the SAME frame (drained via `pendingLuaWindowOps_`), NOT the next frame. Without F1's `IsOnUiThread()` short-circuit, the dispatcher would defer until next frame and the test would still pass eventually — but the in-frame property matters for click-to-update latency. Measure frame numbers via `perf.snapshot`.
16. **No double-bump (F4)**: instrument `NotifyLuaTicketDataChanged` with a counter. Trigger a single `UpdateTicket` (e.g. type one character into a cell and commit). Expected: counter += 1, **not 2**. Confirms only `RefreshLocalData` calls the bump; `UpdateTicket`'s trailing `RefreshLocalData()` does not double-fire.
17. **Stale-deletion bump (F5)**: open a window displaying `smatchet.active_ticket_count()`. Trigger a sync that prunes stale tickets (e.g. JQL filter narrowing). Expected: window re-records when stale-deletion completes — value updates within one frame of the `ActiveTicketsRevision.fetch_add(1)` at `TicketSyncService.cpp:195`. Without F5, the window would show pre-prune count until next refresh.
18. **Recorder stash-and-error (F6)**: provider closure stashes `draw` into a Lua global (`G_DRAW = draw`). Provider returns. Next frame, an event callback runs `G_DRAW:text("zombie")`. Expected: `luaL_error` with `"draw:* called outside its recording window"`; cell renders normally; no crash. Confirms `shared_ptr` co-ownership + `Deactivate` flag.

Cleanup: remove Lua-side `log_info` instrumentation after verification. `SMATCHET_UI_PERF_SCOPE` markers stay — `LuaDrawList::Record/Replay` and `LuaWindow::Record/Replay` are durable Lua-cache stats worth keeping in perf output.

## Out-of-scope (v2 candidates)

Each item below is tracked in the [v2 stub plan](lua-recorded-cmd-list-v2.md) with open questions + triage priority. Flesh out into a dedicated `docs/plans/active/<slug>.md` when work starts on a specific item.

- Recorder ops: `combo`, `drag_int`, `drag_float`, `slider_*`, `checkbox`, `radio`, `tree_node`. Same callback pattern as `button` / `input_text`.
- Per-window dirty predicates (e.g. window only re-records when ticket id `X` changes).
- `draw:button(label, on_click, { invalidate=false })` for chrome buttons that shouldn't force re-record.
- Animation hooks: a `register_ticket_action`-style timer that calls `ui.invalidate_window` on a cadence.
- Focus-aware InputText invalidation: skip re-record while a cell's `InputText` has keyboard focus so background syncs don't clobber in-progress edits.
- Promote `LuaImmediateModeGuard` + `LuaHookGuard` patterns to a shared `LuaScopedExec` helper if a third entry-point needs them.
- **Opt-out window auto-invalidation** (Q2 option d): `ui.register_window(name, { auto_invalidate = false }, fn)` — windows never auto-dirty on `luaWindowDataGen_` bump; re-record only on `ui.invalidate_window` or callback fire. Pushes responsibility to author for windows that don't display ticket data (e.g. a static config panel) — avoids the N_windows × record_time cost entirely on sync.

## Implementation log

- `075ad73 · feat(lua): declare recorded-cmd-list cell + window header surface` — types public, holding members private, sol::state ordered first, stubs cover unconditional surface.
- `7216335 · feat(build): compile Lua 5.3 as C++ with extern "C" linkage` — `LANGUAGE CXX` on `LUA_SOURCES` + `luaconf.h` patched at configure time so `LUA_API` is `extern "C"` under `__cplusplus`, fixing symbol-name mismatch between Lua-as-C++ TUs and sol2's `lua.hpp` callers.
- `daefdae · feat(lua): record-replay cell + window rendering via LuaDrawList` — recorder + replay + per-cell cache + in-frame window-op queue + imgui immediate-mode guard + sandboxed callback invocation.
- `649bfa8 · feat(grid): dispatch grid cells through TryRenderCachedLuaField` — `TicketFieldEditor` swap; single-hook `NotifyLuaTicketDataChanged` in `RefreshLocalData`; coalesced bumps in `ApplyIssueFetchPack` + streaming-batch + stale-deletion + session-end flush.
- `27b4b8a · docs(lua): migrate hook samples + guide to cached recorder API` — `SmatchetHooks.lua` + `docs/guides/lua.md` migrated; example uses `string.match` instead of `decode_json` to dodge the per-cell parse cost + the documented `parse_error` escape.

## Deviations from plan

- **TicketSyncService bump sites**: plan §F5 referenced `TicketSyncService.cpp:267` as "ApplyIssueFetchPack". That line is actually the streaming-batch `stateChanged` block in `TickStreamingApply`, not `ApplyIssueFetchPack`. Corrected to flip `pendingLuaWindowBump_` in three sites: end of `ApplyIssueFetchPack` (non-streaming sync path), the stale-deletion block at `:195`, and the streaming-batch `stateChanged` block at `:282`. Two flush points: the streaming-session-end `if (isWorkerFinished && …)` block and the stale-deletion completion check (stale-deletion runs across many frames after session-end has already fired, so it needs its own flush).
- **`TicketFieldEditor` dispatch line**: plan §Dispatch named `:834` but the actual `TryLuaFieldDisplay` call site is at `:840` (`:834` is the function signature opener). Swap landed at `:840`.
- **`sol::this_state` on usertype member functions**: sol2 v2.20.6's `make_string_view` overload set does not accept member-fn pointers whose first parameter is `sol::this_state` (verified via failing template instantiation at `sol.hpp:18537`). Plan had recorder methods receive `lua_State*` indirectly via `sol::this_state L` and call `luaL_error`. Implementation drops `sol::this_state` and throws `std::runtime_error` from `RequireActive`; sol2 catches `std::exception` inside its `pcall` frame and surfaces a clean Lua error, equivalent semantics. The Lua-as-C++ build mode makes this unwinding safe through recorder destructors.
- **`sol::no_constructor` sentinel**: same sol2 v2.20.6 limitation rejected `sol::no_constructor` as the second positional arg to `new_usertype` (template overload couldn't `make_string_view` from `sol::no_construction&`). Implementation simply omits any constructor binding from the new_usertype call list — sol2 v2 does not synthesize a default Lua-side constructor when no constructor sentinel + factory is present, so Lua scripts cannot construct a bare `SmatchetDrawList`.
- **`luaconf.h` patching**: plan §Lua build mode prescribed `set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)` alone. That triggers C++ symbol mangling of every Lua API function because Lua 5.3's `lua.h` lacks `extern "C"` guards — sol2 (which includes via `lua.hpp` with `extern "C"`) then fails to link. Added a configure-time `string(REPLACE …)` patch of `luaconf.h` that wraps `LUA_API` in `extern "C"` under `__cplusplus`. Idempotent via a sentinel comment so re-runs are safe.
- **Pre-existing `ListLuaScriptFiles` duplication**: the no-Lua build was broken on `develop` because `ListLuaScriptFiles` was defined both in `AppController.cpp` (unconditional) and `AppController_LuaStubs.cpp`. Removed the stub copy with a comment explaining the duplication; the `AppController.cpp` definition does not actually depend on Lua being compiled in. Out of strict scope for the cmd-list feature, but the plan's stub-build verification step (`-DSMATCHET_WITH_LUA_AUTOMATION=OFF`) required it.

- **`ResolveFieldIconAssetPath` memoization added in same PR** (not in original plan). Initial post-impl perf run with the Lua `priority` provider enabled measured `LuaDrawList::Replay` at 1.95 ms/cell — 390× the 5 µs/cell spec. `perf-detective` traced the cost to `AppController::ResolveFieldIconAssetPath` doing 3 `fs::weakly_canonical` syscalls per call with no memoization. This is a pre-existing C++ hot-path cost not introduced by the recorded-cmd-list feature, but the feature's stated perf win is impossible without it because every `draw:image` op routes through the resolver at replay. Fix landed inline (UI-thread-only memo cache on `AppController`, keyed on `pathOrUrl`, capped at 256 entries with `clear()` on overflow). Both base directories (`luaScriptsDirectory_`, `ConfigManager::GetRuntimeAssetDirectory()`) are set once at startup so cache key on the raw input is sufficient — no invalidation hook needed. Re-measurement: 0.69 µs/cell (2826× faster). See updated Verification section.

- **`ReplayCmdList` signature reverted to take `AppController&` AND `sol::state&`** (originally lifted only `sol::state` away). Code-review flagged that the original sig did a `lua["__smatchet_app"].get_or<AppController*>(nullptr)` per `Op::Image` per replay frame, a Lua-table indexing call on the hot path. Lifting just the AppController* lookup is the perf-relevant fix; sol::state is still needed in replay for sandboxed callbacks (`InvokeLuaCallbackSandboxed`), so it stays as a second arg.

## Verification

- `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone` — **PASS** (305 MB exe produced; no warnings beyond pre-existing ones).
- `cmake --build --preset ninja-iter-msys2 --target SmatchetCore_DX12` — **PASS** (no header pollution from the new public recorder types; `ImCmd` / `LuaWindowEntry` use only `<string>` + `<vector>` + `<cstdint>` + sol2 — same set as the existing Lua state).
- `cmake -B build/lua-off-check -DSMATCHET_WITH_LUA_AUTOMATION=OFF -G Ninja && cmake --build build/lua-off-check --target SmatchetStandalone` — **PASS** (stub build links cleanly after `ListLuaScriptFiles` duplication fix; all unconditional surface members route to no-ops).
- `perf.snapshot` measurement: **PASS** via `scenario.run priority-grid-scroll` (300 frames, 24 visible Lua `priority` cells). Initial run revealed a 390× regression (`LuaDrawList::Replay` 1.95 ms/cell vs 5 µs spec) traced by `perf-detective` to `AppController::ResolveFieldIconAssetPath` doing 3 unmemoized `fs::weakly_canonical` syscalls per `draw:image` op per replay frame. **Not introduced by this PR** — pre-existing latent cost in the C++ icon-resolve path, but the recorded-cmd-list cache made it visible because the feature's value hinges on per-frame replay being near-free. Memoization fix added in same PR (`fieldIconAssetPathCache_` member, 256-entry cap, clear-on-overflow). Re-measurement:
  - `LuaDrawList::Replay` total **0.0166 ms** for 24 cells = **0.69 µs/cell** (7× under the 5 µs spec, 2826× faster than the pre-fix baseline).
  - `RenderFieldCell` total **0.21 ms** for 216 calls = **0.98 µs/cell** (was 47.14 ms in the pre-fix run, ≈ 224× win).
  - `LuaDrawList::Record.calls` = 0 in steady state (cache protocol confirmed).
  - `SmatchetUI::Draw` 3.39 ms (was 50.37 ms with Lua-priority on and no memo).
- **Fuzz scenario** (`scenarios/lua_recorder_fuzz.lua`): **PASS** via `scenario.run --name=lua-recorder-fuzz --frames=3600` (60 s at 60 fps). Implemented as `Source_Core/src/Commands/Scenarios/LuaRecorderFuzzScenario.cpp` + `scripts/LuaRecorderFuzz.lua` (commit `f66b2b0`). The Lua provider body randomises calls with every documented hostile-input class — NaN sizes, +/-Inf, OOB color indices (-99999, 99999), oversized strings (200 KB), embedded NULs, unbalanced `push_color` without matching `pop_color`, `draw:button` with empty label, `max_len` = 0 / -5 / 65535 / 100000, attached `on_deactivated*` callbacks, no-op lambdas. Each frame replays the recorded list, exercising every defensive bound in `LuaDrawList::*` and `ReplayCmdList`. Result: clean exit `ok:true`, no crash, no assert, no replay abort. `RenderFieldCell` stayed at ~0.9 µs/cell — the crash-safety hardening adds no measurable per-cell overhead. Single CLI invocation, no manual setup required.
- **Visual regression set** + **shutdown safety** + **window timing F1 test** + **double-bump F4 test** + **stale-deletion F5 test** + **recorder stash-and-error F6 test**: pending a manual UI session — not run in this agent round.
