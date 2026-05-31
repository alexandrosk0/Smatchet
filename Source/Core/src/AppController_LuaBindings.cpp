#include "AppController.h"
#include "ILuaBindingHost.h"
#include "LuaAutomationHost.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "ConfigManager.h"
#include "FieldEditAuditSource.h"
#include "IssueTableSerializer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <exception>
#include <future>
#include <ghc/filesystem.hpp>
#include <limits>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <iterator>

#include <nlohmann/json.hpp>

#include "imgui.h"
#include "Logger.h"
#include "SmatchetFieldIconRender.h"
#include "StringUtil.h"
#include "TicketGridModel.h"
#include "TrackerFieldValueUtils.h"
#include "UiPerfMonitor.h"

#include "AppController_LuaBindings_detail.h"

// ---------------------------------------------------------------------------
// File-scope definitions — shared with AppController_LuaBindings_Draw.cpp via
// AppController_LuaBindings_detail.h.  Must NOT be in an anonymous namespace.
// ---------------------------------------------------------------------------

// Thread-local: false during cached cell / window recording. The imgui.* glue
// checks this and luaL_errors if Lua tries to draw immediate-mode UI while a
// cached recording is active — otherwise the script would draw once on cache
// miss and silently vanish on the next replay.
// Per docs/plans/shipped/lua-recorded-cmd-list.md decision #5 + finding #6.
thread_local bool g_luaImmediateModeAllowed = true;

std::string TruncateForTrace(const std::string& s, std::size_t maxLen) {
    if (s.size() <= maxLen) {
        return s;
    }
    return s.substr(0, maxLen) + "...";
}

bool LuaTruthy(const sol::object& o) {
    if (!o.valid()) {
        return false;
    }
    const sol::type t = o.get_type();
    if (t == sol::type::lua_nil) {
        return false;
    }
    if (t == sol::type::boolean) {
        return o.as<bool>();
    }
    return true;
}

std::string AsciiLowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

namespace {

constexpr int kJsonToLuaMaxDepth = 64;

sol::object JsonToLuaImpl(sol::state_view luaView, const nlohmann::json& j, int depth) {
    if (depth > kJsonToLuaMaxDepth) {
        return sol::make_object(luaView, sol::nil);
    }
    switch (j.type()) {
    case nlohmann::json::value_t::null:
        return sol::make_object(luaView, sol::nil);
    case nlohmann::json::value_t::boolean:
        return sol::make_object(luaView, j.get<bool>());
    case nlohmann::json::value_t::number_integer:
        return sol::make_object(luaView, static_cast<double>(j.get<std::int64_t>()));
    case nlohmann::json::value_t::number_unsigned:
        return sol::make_object(luaView, static_cast<double>(j.get<std::uint64_t>()));
    case nlohmann::json::value_t::number_float:
        return sol::make_object(luaView, j.get<double>());
    case nlohmann::json::value_t::string:
        return sol::make_object(luaView, j.get<std::string>());
    case nlohmann::json::value_t::array: {
        sol::table arr = luaView.create_table();
        std::size_t idx = 1;
        for (const auto& el : j) {
            arr[idx++] = JsonToLuaImpl(luaView, el, depth + 1);
        }
        return arr;
    }
    case nlohmann::json::value_t::object: {
        sol::table tbl = luaView.create_table();
        for (auto it = j.begin(); it != j.end(); ++it) {
            tbl[it.key()] = JsonToLuaImpl(luaView, it.value(), depth + 1);
        }
        return tbl;
    }
    default:
        return sol::make_object(luaView, sol::nil);
    }
}

nlohmann::json LuaToJsonImpl(sol::object obj, int depth) {
    if (depth > 64)
        return nullptr;
    if (obj.get_type() == sol::type::lua_nil)
        return nullptr;
    if (obj.is<bool>())
        return obj.as<bool>();
    if (obj.is<double>())
        return obj.as<double>();
    if (obj.is<std::string>())
        return obj.as<std::string>();
    if (obj.is<sol::table>()) {
        sol::table t = obj.as<sol::table>();
        bool is_array = true;
        size_t max_idx = 0;
        t.for_each([&](sol::object k, sol::object /*value*/) {
            if (k.is<size_t>()) {
                max_idx = (std::max)(max_idx, k.as<size_t>());
            } else {
                is_array = false;
            }
        });
        if (is_array && max_idx > 0) {
            nlohmann::json j = nlohmann::json::array();
            for (size_t i = 1; i <= max_idx; ++i) {
                j.push_back(LuaToJsonImpl(t[i], depth + 1));
            }
            return j;
        } else {
            nlohmann::json j = nlohmann::json::object();
            t.for_each([&](sol::object k, sol::object v) {
                if (k.is<std::string>()) {
                    j[k.as<std::string>()] = LuaToJsonImpl(v, depth + 1);
                }
            });
            return j;
        }
    }
    return nullptr;
}

std::string SanitizeLogText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc == '\t' || uc == '\n') {
            out.push_back(c);
        } else if (uc < 0x20 || uc == 0x7F) {
            out.push_back('?');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/** Flatten Lua values the same way as JSON import cells (strings, numbers, bools, simple arrays). */
static std::string LuaObjectToIssueFieldString(const sol::object& v, std::size_t maxDump = 4096) {
    if (!v.valid() || v.get_type() == sol::type::lua_nil) {
        return std::string();
    }
    if (v.is<bool>()) {
        return v.as<bool>() ? std::string("true") : std::string("false");
    }
    if (v.is<double>()) {
        const double d = v.as<double>();
        if (d == std::floor(d) && d >= static_cast<double>((std::numeric_limits<std::int64_t>::min)()) &&
            d <= static_cast<double>((std::numeric_limits<std::int64_t>::max)())) {
            return std::to_string(static_cast<std::int64_t>(d));
        }
        return std::to_string(d);
    }
    if (v.is<std::string>()) {
        return v.as<std::string>();
    }
    if (v.is<sol::table>()) {
        sol::table tbl = v.as<sol::table>();
        std::size_t maxIdx = 0;
        bool hasNonIntKey = false;
        tbl.for_each([&](sol::object k, sol::object /*val*/) {
            if (hasNonIntKey) {
                return;
            }
            std::size_t idx = 0;
            if (k.is<std::size_t>()) {
                idx = k.as<std::size_t>();
            } else if (k.is<int>()) {
                const int iv = k.as<int>();
                if (iv < 1) {
                    hasNonIntKey = true;
                    return;
                }
                idx = static_cast<std::size_t>(iv);
            } else {
                hasNonIntKey = true;
                return;
            }
            if (idx > maxIdx) {
                maxIdx = idx;
            }
        });
        if (!hasNonIntKey && maxIdx > 0 && maxIdx <= 100000) {
            bool dense = true;
            for (std::size_t i = 1; i <= maxIdx; ++i) {
                const sol::object el = tbl[i];
                if (!el.valid() || el.get_type() == sol::type::lua_nil) {
                    dense = false;
                    break;
                }
            }
            if (dense) {
                std::string joined;
                for (std::size_t i = 1; i <= maxIdx; ++i) {
                    if (!joined.empty()) {
                        joined.push_back(',');
                    }
                    joined += LuaObjectToIssueFieldString(tbl[i], maxDump / (maxIdx + 1));
                }
                return joined;
            }
        }
        try {
            std::string dumped = LuaToJson(v).dump();
            if (dumped.size() > maxDump) {
                return dumped.substr(0, maxDump) + "...";
            }
            return dumped;
        } catch (...) { // catch-all-ok: dump on Lua value for logging
            return std::string("?");
        }
    }
    return std::string();
}

static void LuaApplyIssueCreateKv(IssueDraft& draft, const std::string& rawKey, const std::string& val,
                                  const std::vector<TrackerField>& catalog) {
    const std::string low = AsciiLowerCopy(rawKey);
    if (low == "issuetypeid" || low == "issue_type_id") {
        draft.IssueTypeId = val;
        return;
    }
    if (low == "issuetypename" || low == "issue_type_name") {
        draft.IssueTypeName = val;
        return;
    }
    if (low == "projectkey" || low == "project_key") {
        IssueTableSerializer::ApplyKeyValueToDraft(draft, "project", val);
        return;
    }
    if (low == "parentkey" || low == "parent_key") {
        IssueTableSerializer::ApplyKeyValueToDraft(draft, "parent", val);
        return;
    }
    if (low == "existingissuekey" || low == "existing_issue_key") {
        draft.ExistingIssueKey = val;
        return;
    }
    const std::string resolved = IssueTableSerializer::ResolveColumnKey(rawKey, catalog);
    if (resolved.empty()) {
        return;
    }
    IssueTableSerializer::ApplyKeyValueToDraft(draft, resolved, val);
}

static void LuaMergeIssueCreateSpec(IssueDraft& draft, sol::table spec, const std::vector<TrackerField>& catalog) {
    spec.for_each([&](sol::object kObj, sol::object vObj) {
        if (!kObj.is<std::string>()) {
            return;
        }
        const std::string rawKey = kObj.as<std::string>();
        const std::string low = AsciiLowerCopy(rawKey);
        if (low == "offline" || low == "queue_offline") {
            return;
        }
        if (low == "fields" && vObj.is<sol::table>()) {
            LuaMergeIssueCreateSpec(draft, vObj.as<sol::table>(), catalog);
            return;
        }
        LuaApplyIssueCreateKv(draft, rawKey, LuaObjectToIssueFieldString(vObj), catalog);
    });
}

} // namespace

// File-scope wrappers — call through to the TU-private *Impl helpers above.
// Declared in AppController_LuaBindings_detail.h; visible to _Draw.cpp.

sol::object JsonToLua(sol::state_view luaView, const nlohmann::json& j) { return JsonToLuaImpl(luaView, j, 0); }

nlohmann::json LuaToJson(sol::object obj) { return LuaToJsonImpl(obj, 0); }

sol::environment CreateSandboxEnvironment(sol::state& lua) {
    // Lua semantics gotcha: `sandbox["X"] = nil` is equivalent to `rawset(sandbox, "X", nil)`
    // which REMOVES the key, allowing the metatable `__index = lua.globals()` fallback to
    // resolve to the *real* function. So nilling doesn't block — it merely unbinds locally.
    // Use `false` (a non-nil-but-non-callable sentinel) so direct lookups hit it AND any
    // `something()` call errors with "attempt to call a boolean value".
    sol::environment sandbox(lua, sol::create, lua.globals());
    const auto block = [&](const char* name) { sandbox[name] = false; };
    // Sandbox escapes — primitives that load / execute external code.
    block("dofile");
    block("loadfile");
    block("load");
    block("loadstring");
    block("require");
    block("collectgarbage");
    block("io");      // file / process I/O
    block("package"); // module loader (could load shared libs)
    block("debug");   // bytecode / locals introspection
    // Bytecode dump is mostly inert without `load`, but strip as defense-in-depth.
    sandbox["string"] = lua.globals()["string"]; // shadow + then patch the local copy
    // Cannot mutate the shared global `string` table (would leak to non-sandboxed paths
    // and break Lua-internal users of string.dump). Build a per-sandbox copy with the
    // dangerous fns blocked. Cheap — string is a small table of function refs.
    sol::table stringSafe = lua.create_table();
    sol::table stringGlobal = lua.globals()["string"];
    if (stringGlobal.valid()) {
        for (auto& kv : stringGlobal) {
            const std::string key = kv.first.as<std::string>();
            if (key == "dump")
                continue; // strip bytecode dumper
            stringSafe[key] = kv.second;
        }
    }
    sandbox["string"] = stringSafe;
    // Strict mode (defense-in-depth): metatable + raw-table accessors. A script with
    // these can hijack the sandbox env's bindings — `rawset(_G, "log_info", fake)` would
    // replace the log binding. Hook patterns don't need these.
    block("setmetatable");
    block("getmetatable");
    block("rawset");
    block("rawget");
    block("rawequal");
    block("rawlen");
    // `os` is intentionally NOT blocked — InitLuaCore replaced the standard lib with a
    // whitelist of safe time/date functions (time, clock, difftime, date). The global
    // `os` table is the safe one; the sandbox metatable falls through to it naturally.
    return sandbox;
}

/** GCC + sol2: lambdas with the same signature can share one demangled metatable name → heap corruption in Lua.
 *  All InitLua callables here are plain functions with distinct symbols (plus scoped AppController* for Ticket glue).
 */
namespace smatchet_lua_init_detail {

// UI glues resolve through `__smatchet_app_ui` (an AppController*), set by
// InitLuaUi. The Core key `__smatchet_app` stores an `ILuaBindingHost*` after
// the interface lift (see ILuaBindingHost.h + AppController_LuaBindingsCore.cpp).
// Sol2 v2.20.6's `get<T*>` does not perform a type-safe downcast through
// multiple inheritance offsets, so resolving an AppController* from a
// stored ILuaBindingHost* would corrupt with the wrong base offset. The
// dedicated UI key keeps the cast site straightforward.
static AppController* ResolveApp(sol::this_state L) {
    sol::state_view lua(L);
    const sol::object appObj = lua["__smatchet_app_ui"];
    if (!appObj.valid() || appObj.get_type() == sol::type::lua_nil) {
        return nullptr;
    }
    return appObj.as<AppController*>();
}

// TicketSetFieldGlue / TicketTransitionGlue lifted to AppController_LuaBindingsCore.cpp
// (ImGui-free TU) so test binaries can exercise the binding round-trip without ImGui.
// See ILuaBindingHost.h. The lifted definitions resolve through ILuaBindingHost*
// instead of AppController*.

// imgui.* glue: each entry rejects when called inside a cached provider / window recording.
// `g_luaImmediateModeAllowed` is flipped false by LuaImmediateModeGuard around the
// `TryRenderCachedLuaField` provider call and the `DrawLuaWindows` record path; only
// event-time callbacks (`register_ticket_action`, `register_global_action`, MCP tools)
// leave it true. luaL_error unwinds via C++ exceptions thanks to the Lua-as-C++ build mode.
void ImGuiSameLineGlue(sol::this_state L) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return;
    }
    ImGui::SameLine();
}

void ImGuiSeparatorGlue(sol::this_state L) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return;
    }
    ImGui::Separator();
}

void ImGuiProgressBarGlue(sol::this_state L, float fraction, float width, float height) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return;
    }
    ImVec2 sz(width, height);
    if (width < 0.0f) {
        sz.x = ImGui::GetContentRegionAvail().x;
    }
    if (height <= 0.0f) {
        sz.y = ImGui::GetFrameHeight();
    }
    ImGui::ProgressBar(fraction, sz);
}

std::tuple<float, float> ImGuiGetContentRegionAvailGlue(sol::this_state L) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return std::make_tuple(0.0f, 0.0f);
    }
    const ImVec2 v = ImGui::GetContentRegionAvail();
    return std::make_tuple(v.x, v.y);
}

bool ImGuiButtonGlue(sol::this_state L, const std::string& label) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return false;
    }
    return ImGui::Button(label.c_str());
}

// LuaLogInfoGlue / LuaGetTicketGlue / LuaGetActiveTicketsGlue / LuaDecodeJsonGlue
// lifted to AppController_LuaBindingsCore.cpp.

void LuaRegisterFieldDisplayCachedGlue(sol::this_state L, const std::string& fieldId, sol::function fn) {
    AppController* app = ResolveApp(L);
    if (app)
        app->LuaRegisterFieldDisplayCachedBind(fieldId, std::move(fn));
}

void LuaUnregisterFieldDisplayCachedGlue(sol::this_state L, const std::string& fieldId) {
    AppController* app = ResolveApp(L);
    if (app)
        app->LuaUnregisterFieldDisplayCachedBind(fieldId);
}

void LuaRegisterFieldDisplayCachedByNameGlue(sol::this_state L, const std::string& displayName, sol::function fn) {
    AppController* app = ResolveApp(L);
    if (app)
        app->LuaRegisterFieldDisplayCachedByNameBind(displayName, std::move(fn));
}

void LuaUnregisterFieldDisplayCachedByNameGlue(sol::this_state L, const std::string& displayName) {
    AppController* app = ResolveApp(L);
    if (app)
        app->LuaUnregisterFieldDisplayCachedByNameBind(displayName);
}

void LuaUiInvalidateWindowGlue(sol::this_state L, const std::string& name) {
    AppController* app = ResolveApp(L);
    if (app)
        app->LuaUiInvalidateWindowBind(name);
}

void LuaUiInvalidateFieldCacheGlue(sol::this_state L, sol::optional<std::string> ticketId,
                                   sol::optional<std::string> fieldId) {
    AppController* app = ResolveApp(L);
    if (app)
        app->LuaUiInvalidateFieldCacheBind(ticketId, fieldId);
}

void LuaUiUnregisterWindowGlue(sol::this_state L, const std::string& name) {
    AppController* app = ResolveApp(L);
    if (app)
        app->LuaUiUnregisterWindowBind(name);
}

void LuaRegisterFieldIconMapGlue(sol::this_state L, const std::string& fieldKey, sol::table map,
                                 sol::optional<bool> byName) {
    AppController* app = ResolveApp(L);
    if (app)
        app->LuaRegisterFieldIconMapBind(fieldKey, std::move(map), byName);
}

void LuaUnregisterFieldIconMapGlue(sol::this_state L, const std::string& fieldKey, sol::optional<bool> byName) {
    AppController* app = ResolveApp(L);
    if (app)
        app->LuaUnregisterFieldIconMapBind(fieldKey, byName);
}

void LuaImGuiTextGlue(sol::this_state L, const std::string& s) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return;
    }
    AppController* app = ResolveApp(L);
    if (app)
        app->LuaImGuiTextBind(s);
}

void LuaImGuiTextUnformattedGlue(sol::this_state L, const std::string& s) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return;
    }
    AppController* app = ResolveApp(L);
    if (app)
        app->LuaImGuiTextUnformattedBind(s);
}

bool LuaImGuiImageGlue(sol::this_state L, const std::string& path, float w, float h) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return false;
    }
    AppController* app = ResolveApp(L);
    return app ? app->LuaImGuiImageBind(path, w, h) : false;
}

void LuaUiRegisterWindowGlue(sol::this_state L, const std::string& name, sol::function drawFn) {
    AppController* app = ResolveApp(L);
    if (app)
        app->LuaUiRegisterWindowBind(name, std::move(drawFn));
}

void LuaUiRegisterTicketActionGlue(sol::this_state L, const std::string& name, const std::string& cb) {
    AppController* app = ResolveApp(L);
    if (app)
        app->LuaUiRegisterTicketActionBind(name, cb);
}

void LuaUiRegisterGlobalActionGlue(sol::this_state L, const std::string& name, const std::string& cb) {
    AppController* app = ResolveApp(L);
    if (app)
        app->LuaUiRegisterGlobalActionBind(name, cb);
}

// --- AI assistant glues (Phase E) ----------------------------------------------
// Resolve via `__smatchet_app_ui` (AppController*) and call the always-on stubs
// `AddAiContext` / `ClearAiContext` / `PromptAi` shipped Phase B. Those stubs
// no-op when `SMATCHET_WITH_AI=0`, so the glues need no extra gating here.
//
// **Threading expectation**: Lua scripts driving `ai.*` are expected to run on
// the UI thread (the main `lua` state). The background automation worker reuses
// `__smatchet_app_ui` on its per-iteration `bgState` — calling `ai.*` from a
// worker script will race-mutate `aiAssistant_->luaContext_`. That is a Phase B
// design choice (no MainThreadDispatcher hop) inherited here, not introduced by
// Phase E. SmatchetHooks.lua only calls `ai.*` from UI-event paths today.

// Build an `AiContextBlock` from a Lua table { name=string, body=string,
// kind=("active_ticket"|"multi_selected_tickets"|"visible_grid_rows"|
//        "active_view"|"audit_trail") }. Missing/invalid kind defaults to
// `ActiveTicket` (matches `AiContextBlock` default ctor).
static AiContextBlock LuaTableToAiContextBlock(const sol::table& tbl) {
    AiContextBlock block;
    const sol::object nameObj = tbl["name"];
    if (nameObj.valid() && nameObj.is<std::string>()) {
        block.Name = nameObj.as<std::string>();
    }
    const sol::object bodyObj = tbl["body"];
    if (bodyObj.valid() && bodyObj.is<std::string>()) {
        block.Body = bodyObj.as<std::string>();
    }
    const sol::object kindObj = tbl["kind"];
    if (kindObj.valid() && kindObj.is<std::string>()) {
        const std::string k = kindObj.as<std::string>();
        if (k == "multi_selected_tickets") {
            block.Kind = AiContextBlockKind::MultiSelectedTickets;
        } else if (k == "visible_grid_rows") {
            block.Kind = AiContextBlockKind::VisibleGridRows;
        } else if (k == "active_view") {
            block.Kind = AiContextBlockKind::ActiveView;
        } else if (k == "audit_trail") {
            block.Kind = AiContextBlockKind::AuditTrail;
        } else {
            block.Kind = AiContextBlockKind::ActiveTicket; // also covers "active_ticket"
        }
    }
    return block;
}

void LuaAiAddContextGlue(sol::this_state L, sol::table blockTbl) {
    AppController* app = ResolveApp(L);
    if (!app)
        return;
    app->AddAiContext(LuaTableToAiContextBlock(blockTbl));
}

void LuaAiClearContextGlue(sol::this_state L) {
    AppController* app = ResolveApp(L);
    if (app)
        app->ClearAiContext();
}

void LuaAiPromptGlue(sol::this_state L, const std::string& prompt, sol::optional<sol::table> extraBlocks) {
    AppController* app = ResolveApp(L);
    if (!app)
        return;
    // Optional extra context blocks: appended to the controller's context vector
    // before Submit, matching the panel's "Send-with-context" path. Each element
    // is treated as an `AiContextBlock` table.
    if (extraBlocks) {
        sol::table arr = extraBlocks.value();
        for (std::size_t i = 1;; ++i) {
            const sol::object el = arr[i];
            if (!el.valid() || el.get_type() == sol::type::lua_nil)
                break;
            if (!el.is<sol::table>())
                break;
            app->AddAiContext(LuaTableToAiContextBlock(el.as<sol::table>()));
        }
    }
    app->PromptAi(prompt);
}

// LuaCommandsInvokeGlue / LuaMcpRegisterToolGlue / LuaCreateIssueGlue /
// LuaTrackerGetTypeGlue / LuaTrackerCreateIssueGlue lifted to
// AppController_LuaBindingsCore.cpp (ImGui-free TU).

} // namespace smatchet_lua_init_detail

void AppController::InitLua() {
    InitLuaCore(lua);
    InitLuaUi(lua);
}

// Lifted to `smatchet::lua::InitLuaCore(state, host)` in AppController_LuaBindingsCore.cpp.
// This forwarder keeps the existing call sites (`AppController::InitLua` + the
// AutomationWorkerLoop `InitLuaCore(bgState)`) source-compatible. The host
// pointer passed through is `this` (AppController inherits from ILuaBindingHost
// when SMATCHET_WITH_LUA_AUTOMATION is on).
void AppController::InitLuaCore(sol::state& state) { smatchet::lua::InitLuaCore(state, this); }

void AppController::InitLuaUi(sol::state& state) {
    // Mirror of `__smatchet_app` (ILuaBindingHost*) -- this slot holds the concrete
    // AppController* so UI glues in this TU (which still call AppController-only
    // members like LuaRegisterFieldDisplayCachedBind / LuaUiRegisterWindowBind)
    // can resolve through ResolveApp without a multiple-inheritance offset hazard.
    // Sol2 v2.20.6's `get<T*>` does not retag through base offsets, so storing
    // both pointers explicitly is safer than downcasting at lookup time.
    state["__smatchet_app_ui"] = this;

    // Cached-cmd-list cell renderer. Provider receives a 7th arg `draw` -- a recorder; the
    // returned recording replays every frame until cache-key inputs change. See plan §Cells.
    state.set_function("register_field_display_cached", &smatchet_lua_init_detail::LuaRegisterFieldDisplayCachedGlue);
    state.set_function("unregister_field_display_cached",
                       &smatchet_lua_init_detail::LuaUnregisterFieldDisplayCachedGlue);
    state.set_function("register_field_display_cached_by_name",
                       &smatchet_lua_init_detail::LuaRegisterFieldDisplayCachedByNameGlue);
    state.set_function("unregister_field_display_cached_by_name",
                       &smatchet_lua_init_detail::LuaUnregisterFieldDisplayCachedByNameGlue);
    state.set_function("register_field_icon_map", &smatchet_lua_init_detail::LuaRegisterFieldIconMapGlue);
    state.set_function("unregister_field_icon_map", &smatchet_lua_init_detail::LuaUnregisterFieldIconMapGlue);

    // Recorder usertype: only methods on this object may be called inside cached providers
    // and window draw fns. Constructors disabled — the C++ side hands a shared_ptr to Lua at
    // recording time and Deactivate()s it when done. Stashing the recorder past that point
    // errors cleanly via the active_ flag.
    // Lua-side construction of SmatchetDrawList is intentionally not exposed; C++ hands the
    // recorder to providers via std::shared_ptr at recording time. Omitting `sol::call_constructor`
    // is enough — sol2 won't synthesize a constructor binding from this list.
    state.new_usertype<LuaDrawList>(
        "SmatchetDrawList", "text", &LuaDrawList::Text, "text_unformatted", &LuaDrawList::TextUnformatted, "image",
        &LuaDrawList::Image, "progress_bar", &LuaDrawList::ProgressBar, "same_line", &LuaDrawList::SameLine,
        "separator", &LuaDrawList::Separator, "dummy", &LuaDrawList::Dummy, "push_color", &LuaDrawList::PushColor,
        "pop_color", &LuaDrawList::PopColor, "set_tooltip", &LuaDrawList::SetTooltip, "button", &LuaDrawList::Button,
        "input_text", &LuaDrawList::InputText, "on_deactivated", &LuaDrawList::OnDeactivated,
        "on_deactivated_after_edit", &LuaDrawList::OnDeactivatedAfterEdit);

    sol::table imgui = state.create_table();
    imgui.set_function("progress_bar", &smatchet_lua_init_detail::ImGuiProgressBarGlue);
    imgui.set_function("text", &smatchet_lua_init_detail::LuaImGuiTextGlue);
    imgui.set_function("text_unformatted", &smatchet_lua_init_detail::LuaImGuiTextUnformattedGlue);
    imgui.set_function("get_content_region_avail", &smatchet_lua_init_detail::ImGuiGetContentRegionAvailGlue);
    imgui.set_function("button", &smatchet_lua_init_detail::ImGuiButtonGlue);
    imgui.set_function("same_line", &smatchet_lua_init_detail::ImGuiSameLineGlue);
    imgui.set_function("separator", &smatchet_lua_init_detail::ImGuiSeparatorGlue);
    imgui.set_function("image", &smatchet_lua_init_detail::LuaImGuiImageGlue);
    state["imgui"] = imgui;

    sol::table ui = state.create_table();
    ui.set_function("register_window", &smatchet_lua_init_detail::LuaUiRegisterWindowGlue);
    ui.set_function("unregister_window", &smatchet_lua_init_detail::LuaUiUnregisterWindowGlue);
    ui.set_function("invalidate_window", &smatchet_lua_init_detail::LuaUiInvalidateWindowGlue);
    ui.set_function("invalidate_field_cache", &smatchet_lua_init_detail::LuaUiInvalidateFieldCacheGlue);
    ui.set_function("invalidate_field_cache_for", &smatchet_lua_init_detail::LuaUiInvalidateFieldCacheGlue);
    ui.set_function("register_ticket_action", &smatchet_lua_init_detail::LuaUiRegisterTicketActionGlue);
    ui.set_function("register_global_action", &smatchet_lua_init_detail::LuaUiRegisterGlobalActionGlue);
    state["ui"] = ui;

    // Phase E: `ai.*` surface. Registered here (not in `InitLuaCore`) because the
    // glues resolve through `__smatchet_app_ui` (an `AppController*`); the Core
    // table only stores an `ILuaBindingHost*`. See `ResolveApp` note above.
    sol::table aiTbl = state.create_table();
    aiTbl.set_function("add_context", &smatchet_lua_init_detail::LuaAiAddContextGlue);
    aiTbl.set_function("clear_context", &smatchet_lua_init_detail::LuaAiClearContextGlue);
    aiTbl.set_function("prompt", &smatchet_lua_init_detail::LuaAiPromptGlue);
    state["ai"] = aiTbl;
}

void AppController::LuaLogInfoBind(const std::string& msg) {
    const std::string clean = SanitizeLogText(msg);
    if (luaHost_ && !luaHost_->SnapshotLogSinks().empty()) {
        for (const auto& sink : luaHost_->SnapshotLogSinks()) {
            sink(clean);
        }
    } else {
        LOG_INFO("[LUA] %s", clean.c_str());
    }
}

std::vector<CachedTicket> AppController::LuaGetActiveTicketsBind() {
    const auto snap = GetActiveTicketsSnapshot();
    return std::vector<CachedTicket>(snap->begin(), snap->end());
}

std::tuple<sol::object, std::string> AppController::LuaGetTicketBind(sol::state_view sv, const std::string& issueId) {
    // Marshal against the *calling* state `sv`, not the member `lua`: the caller may be an
    // off-UI-thread fresh state (MCP / automation worker). Touching `lua` here would re-introduce
    // cross-thread lua_State access + a cross-state sol::object return. See
    // docs/plans/shipped/mcp-lua-fresh-state-race.md.
    CachedTicket ticket;
    if (Cache->TryGetTicket(issueId, ticket)) {
        return {sol::make_object(sv, ticket), ""};
    }
    return {sol::make_object(sv, sol::nil), "Ticket not found in local cache"};
}

std::tuple<sol::object, std::string> AppController::LuaDecodeJsonBind(sol::state_view sv, const std::string& s) {
    // Marshal against the calling state `sv` (see LuaGetTicketBind).
    constexpr size_t kMaxDecodeBytes = 4u * 1024u * 1024u;
    if (s.size() > kMaxDecodeBytes) {
        return {sol::make_object(sv, sol::nil), std::string("input too large")};
    }
    try {
        const nlohmann::json j =
            nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/false);
        return {JsonToLua(sv, j), std::string()};
    } catch (const std::exception& e) {
        return {sol::make_object(sv, sol::nil), std::string(e.what())};
    }
}

std::tuple<sol::object, std::string> AppController::LuaCreateIssueBind(sol::state_view sv, sol::table spec) {
    // Marshal against the calling state `sv`, not the member `lua` (see LuaGetTicketBind):
    // `spec` is already on `sv`, and the result table must be too.
    const TrackerConfig cfg = ConfigManager::Load();
    // Same base as the grid new-issue row: config fallbacks plus last-row project / issue type when present.
    IssueDraft draft = BuildDraftFromLastTicket(cfg);

    sol::object offlineObj = spec["offline"];
    if (!offlineObj.valid() || offlineObj.get_type() == sol::type::lua_nil) {
        offlineObj = spec["queue_offline"];
    }
    const bool offline = LuaTruthy(offlineObj);

    LuaMergeIssueCreateSpec(draft, std::move(spec), AvailableFields);

    sol::table result = sv.create_table();

    if (offline) {
        if (!Cache) {
            return {sol::make_object(sv, sol::nil),
                    std::string("Local cache not initialized (cannot queue offline create)")};
        }
        const std::int64_t qid = QueueCreateOffline(draft);
        if (qid <= 0) {
            result["ok"] = false;
            result["error"] = std::string("Failed to queue offline create (see logs)");
            return {result, std::string()};
        }
        result["ok"] = true;
        result["offline_queued_id"] = static_cast<double>(qid);
        result["issue_key"] = std::string("offline:") + std::to_string(qid);
        result["error"] = std::string();
        return {result, std::string()};
    }

    std::future<IssueCreateResult> fut = CreateIssueAsync(draft);
    IssueCreateResult r;
    try {
        r = fut.get();
    } catch (const std::exception& e) {
        return {sol::make_object(sv, sol::nil),
                std::string("create_issue failed while waiting for result: ") + e.what()};
    } catch (...) { // catch-all-ok: future::get may throw any type; surface as a clean error string
        return {sol::make_object(sv, sol::nil),
                std::string("create_issue failed while waiting for result: unknown exception")};
    }

    result["ok"] = r.Ok;
    if (!r.IssueKey.empty()) {
        result["issue_key"] = r.IssueKey;
    }
    result["error"] = r.Error;
    if (!r.MissingFieldIds.empty()) {
        sol::table miss = sv.create_table();
        std::size_t i = 1;
        for (const auto& id : r.MissingFieldIds) {
            miss[i++] = id;
        }
        result["missing_field_ids"] = miss;
    }
    if (!r.AttachmentFailures.empty()) {
        sol::table af = sv.create_table();
        std::size_t i = 1;
        for (const auto& p : r.AttachmentFailures) {
            sol::table row = sv.create_table();
            row["path"] = p.first;
            row["reason"] = p.second;
            af[i++] = row;
        }
        result["attachment_failures"] = af;
    }
    return {result, std::string()};
}

void AppController::LuaRegisterFieldDisplayCachedBind(const std::string& fieldId, sol::function fn) {
    if (fieldId.empty() || !fn.valid()) {
        return;
    }
    fieldDisplayCachedProviders_[fieldId] = sol::protected_function(std::move(fn));
    // Bump invalidates every cached entry that holds a (possibly-stale) provider ref. Per-entry
    // input comparison in TryRenderCachedLuaField handles ordinary value changes; this is the
    // explicit "registration churn happened" channel.
    luaProviderGen_.fetch_add(1);
}

void AppController::LuaUnregisterFieldDisplayCachedBind(const std::string& fieldId) {
    fieldDisplayCachedProviders_.erase(fieldId);
    luaProviderGen_.fetch_add(1);
}

void AppController::LuaRegisterFieldDisplayCachedByNameBind(const std::string& displayName, sol::function fn) {
    if (displayName.empty() || !fn.valid()) {
        return;
    }
    fieldDisplayCachedProvidersByName_[AsciiLowerCopy(displayName)] = sol::protected_function(std::move(fn));
    luaProviderGen_.fetch_add(1);
}

void AppController::LuaUnregisterFieldDisplayCachedByNameBind(const std::string& displayName) {
    fieldDisplayCachedProvidersByName_.erase(AsciiLowerCopy(displayName));
    luaProviderGen_.fetch_add(1);
}

void AppController::LuaRegisterFieldIconMapBind(const std::string& fieldKey, sol::table map,
                                                sol::optional<bool> byName) {
    if (fieldKey.empty() || !map.valid()) {
        return;
    }
    std::unordered_map<std::string, std::string> inner;
    map.for_each([&](sol::object kObj, sol::object vObj) {
        if (kObj.is<std::string>() && vObj.is<std::string>()) {
            inner[ToLowerAsciiCopy(kObj.as<std::string>())] = vObj.as<std::string>();
        }
    });
    std::lock_guard<std::mutex> lock(fieldIconMapsMutex_);
    if (byName.value_or(false)) {
        fieldIconMapsByDisplayName_[ToLowerAsciiCopy(fieldKey)] = std::move(inner);
    } else {
        fieldIconMapsByFieldId_[fieldKey] = std::move(inner);
    }
}

void AppController::LuaUnregisterFieldIconMapBind(const std::string& fieldKey, sol::optional<bool> byName) {
    std::lock_guard<std::mutex> lock(fieldIconMapsMutex_);
    if (byName.value_or(false)) {
        fieldIconMapsByDisplayName_.erase(ToLowerAsciiCopy(fieldKey));
    } else {
        fieldIconMapsByFieldId_.erase(fieldKey);
    }
}

void AppController::LuaImGuiTextBind(const std::string& s) { ImGui::TextUnformatted(s.c_str()); }

void AppController::LuaImGuiTextUnformattedBind(const std::string& s) { ImGui::TextUnformatted(s.c_str()); }

bool AppController::LuaImGuiImageBind(const std::string& path, float w, float h) {
    return SmatchetFieldIconRender::DrawImagePathOrUrl(*this, path, w, h);
}

void AppController::ApplyOrQueueLuaWindowOp(PendingLuaWindowOp op) {
    // UI-thread-only. See plan §Explicit window invalidation (F1).
    if (inDrawLuaWindows_) {
        pendingLuaWindowOps_.push_back(std::move(op));
        return;
    }
    switch (op.kind) {
    case PendingLuaWindowOp::Kind::Invalidate: {
        auto it = std::find_if(luaWindows_.begin(), luaWindows_.end(),
                               [&](const LuaWindowEntry& w) { return w.name == op.name; });
        if (it != luaWindows_.end()) {
            it->dirty = true;
            it->hasError = false;
            it->errorMessage.clear();
            return;
        }
        break;
    }
    case PendingLuaWindowOp::Kind::Register: {
        auto it = std::find_if(luaWindows_.begin(), luaWindows_.end(),
                               [&](const LuaWindowEntry& w) { return w.name == op.name; });
        if (it != luaWindows_.end())
            luaWindows_.erase(it);
        LuaWindowEntry e;
        e.name = op.name;
        e.drawFn = std::move(op.drawFn);
        e.dirty = true;
        luaWindows_.push_back(std::move(e));
        break;
    }
    case PendingLuaWindowOp::Kind::Unregister:
        luaWindows_.erase(std::remove_if(luaWindows_.begin(), luaWindows_.end(),
                                         [&](const LuaWindowEntry& w) { return w.name == op.name; }),
                          luaWindows_.end());
        break;
    }
}

void AppController::LuaUiRegisterWindowBind(const std::string& name, sol::function drawFn) {
    PendingLuaWindowOp op;
    op.kind = PendingLuaWindowOp::Kind::Register;
    op.name = name;
    if (drawFn.valid()) {
        op.drawFn = sol::protected_function(std::move(drawFn));
    }
    // F1 timing: on-UI uses the in-frame queue so a callback fired inside DrawLuaWindows
    // takes effect this frame, not the next. Off-UI hops the dispatcher first.
    if (IsOnUiThread()) {
        ApplyOrQueueLuaWindowOp(std::move(op));
    } else {
        AppController* self = this;
        mainThreadDispatcher.PostToMainThread(
            [self, opCap = std::move(op)]() mutable { self->ApplyOrQueueLuaWindowOp(std::move(opCap)); });
    }
}

void AppController::LuaUiUnregisterWindowBind(const std::string& name) {
    PendingLuaWindowOp op;
    op.kind = PendingLuaWindowOp::Kind::Unregister;
    op.name = name;
    if (IsOnUiThread()) {
        ApplyOrQueueLuaWindowOp(std::move(op));
    } else {
        AppController* self = this;
        mainThreadDispatcher.PostToMainThread(
            [self, opCap = std::move(op)]() mutable { self->ApplyOrQueueLuaWindowOp(std::move(opCap)); });
    }
}

void AppController::LuaUiInvalidateWindowBind(const std::string& name) {
    PendingLuaWindowOp op;
    op.kind = PendingLuaWindowOp::Kind::Invalidate;
    op.name = name;
    if (IsOnUiThread()) {
        ApplyOrQueueLuaWindowOp(std::move(op));
    } else {
        AppController* self = this;
        mainThreadDispatcher.PostToMainThread(
            [self, opCap = std::move(op)]() mutable { self->ApplyOrQueueLuaWindowOp(std::move(opCap)); });
    }
}

void AppController::LuaUiInvalidateFieldCacheBind(sol::optional<std::string> ticketId,
                                                  sol::optional<std::string> fieldId) {
    const std::string tid = ticketId.value_or(std::string());
    const std::string fid = fieldId.value_or(std::string());
    const bool hasTicket = static_cast<bool>(ticketId);
    const bool hasField = static_cast<bool>(fieldId);
    auto apply = [this, hasTicket, hasField, tid, fid]() {
        if (!hasTicket) {
            luaFieldCache_.clear();
            return;
        }
        if (!hasField) {
            for (auto it = luaFieldCache_.begin(); it != luaFieldCache_.end();) {
                const std::string& key = it->first;
                const std::size_t nul = key.find('\0');
                if (nul != std::string::npos && key.compare(0, nul, tid) == 0)
                    it = luaFieldCache_.erase(it);
                else
                    ++it;
            }
            return;
        }
        std::string key = tid;
        key.push_back('\0');
        key.append(fid);
        luaFieldCache_.erase(key);
    };
    if (IsOnUiThread()) {
        apply();
    } else {
        mainThreadDispatcher.PostToMainThread(std::move(apply));
    }
}

void AppController::NotifyLuaTicketDataChanged() { luaWindowDataGen_.fetch_add(1); }

void AppController::LuaUiRegisterTicketActionBind(const std::string& name, const std::string& callbackFuncName) {
    std::lock_guard<std::mutex> lock(luaActionsMutex_);
    luaTicketActions_.erase(
        std::remove_if(luaTicketActions_.begin(), luaTicketActions_.end(),
                       [&](const std::pair<std::string, std::string>& p) { return p.first == name; }),
        luaTicketActions_.end());
    if (!callbackFuncName.empty()) {
        luaTicketActions_.push_back({name, callbackFuncName});
    }
}

void AppController::LuaUiRegisterGlobalActionBind(const std::string& name, const std::string& callbackFuncName) {
    {
        std::lock_guard<std::mutex> lock(luaActionsMutex_);
        luaGlobalActions_.erase(
            std::remove_if(luaGlobalActions_.begin(), luaGlobalActions_.end(),
                           [&](const std::pair<std::string, std::string>& p) { return p.first == name; }),
            luaGlobalActions_.end());
        if (!callbackFuncName.empty()) {
            luaGlobalActions_.push_back({name, callbackFuncName});
        }
    }
    // commandRegistry_ has its own internal locking; do NOT hold luaActionsMutex_ across this
    // call. ExecuteLuaGlobalAction's handler closure does not re-enter luaActionsMutex_.

    // Mirror into the unified command registry as a `lua.<name>` command so it is
    // discoverable via CLI / MCP / Palette without extra registration. See plan §Lua.
    if (commandRegistry_) {
        const std::string cmdName = "lua." + name;
        // De-dup: if the action was already registered (e.g. script reloaded), remove the old one
        // from the registry. There is no `Unregister` API (registrations are permanent for safety),
        // so we skip re-registration when the exact name is already present.
        if (!commandRegistry_->HasExact(cmdName) && !name.empty() && !callbackFuncName.empty()) {
            smatchet::cmd::Command c;
            c.Name = cmdName;
            c.Category = "lua";
            c.Summary = "(Lua) " + name;
            c.Description = "Lua global action registered via ui.register_global_action(\"" + name + "\", ...).";
            c.Idempotent = false; // Lua actions may mutate state
            c.AsyncSafe = true;
            // Capture by value so the handler owns a copy of the callback name string.
            const std::string cbName = callbackFuncName;
            AppController* appPtr = this;
            c.Handler = [appPtr, cbName](const nlohmann::json& /*args*/, const smatchet::cmd::CommandContext& /*ctx*/) {
                appPtr->ExecuteLuaGlobalAction(cbName);
                return smatchet::cmd::CommandResult::Success(nlohmann::json::object());
            };
            try {
                commandRegistry_->Register(std::move(c));
            } catch (const std::exception& ex) {
                LOG_WARN("LuaUiRegisterGlobalActionBind: could not register '%s' in registry: %s", cmdName.c_str(),
                         ex.what());
            }
        }
    }
}

void AppController::ParseMcpToolDef(const sol::table& toolDef, McpToolDefinition& out) {
    out.name = toolDef.get_or<std::string>("name", "");
    out.description = toolDef.get_or<std::string>("description", "");

    sol::object params = toolDef["parameters"];
    if (params.is<sol::table>()) {
        out.parametersSchema = LuaToJson(params);
    } else {
        std::string schemaStr = toolDef.get_or<std::string>("parameters_json", "{}");
        try {
            out.parametersSchema = nlohmann::json::parse(schemaStr);
        } catch (...) { // catch-all-ok: parse of Lua-provided schema string; fall back to empty schema
            LOG_DEBUG("Lua MCP tool: parameters_json parse failed; using empty schema");
            out.parametersSchema = nlohmann::json::object();
        }
    }
}

void AppController::LuaMcpRegisterToolBind(sol::table toolDef, sol::function callback) {
    if (!toolDef.valid() || !callback.valid()) {
        return;
    }
    McpToolDefinition def;
    ParseMcpToolDef(toolDef, def);
    def.callback = sol::protected_function(std::move(callback));

    std::lock_guard<std::mutex> lock(luaMcpToolsMutex_);
    luaMcpTools_.erase(std::remove_if(luaMcpTools_.begin(), luaMcpTools_.end(),
                                      [&](const McpToolDefinition& d) { return d.name == def.name; }),
                       luaMcpTools_.end());

    luaMcpTools_.push_back(std::move(def));
}

void AppController::ClearLuaTicketContextGlue() {
    // Clear every container that holds sol::protected_function refs BEFORE nulling the
    // __smatchet_app pointer. RAII reverse-declaration destruction in ~AppController already
    // destroys these containers before `lua` (member-order invariant in AppController.h), but
    // belt-and-suspenders: explicitly drop the handles here so a future re-ordering can't
    // turn this into a UAF. See plan §Shutdown ordering.
    luaFieldCache_.clear();
    fieldDisplayCachedProviders_.clear();
    fieldDisplayCachedProvidersByName_.clear();
    luaWindows_.clear();
    pendingLuaWindowOps_.clear();
    lua["__smatchet_app"] = sol::lua_nil;
    lua["__smatchet_app_ui"] = sol::lua_nil;
}

void AppController::RunAutoScript(const std::string& scriptPath, const std::vector<std::string>& selectedIds) {
    std::lock_guard<std::mutex> lock(automationJobMutex_);
    automationJobs_.push_back({AutomationJob::Type::RunAutoScript, scriptPath, selectedIds, ""});
    automationJobCv_.notify_one();
}

void AppController::RunFlatScriptAsync(const std::string& scriptPath) {
    std::lock_guard<std::mutex> lock(automationJobMutex_);
    automationJobs_.push_back({AutomationJob::Type::RunFlatScript, scriptPath, {}, ""});
    automationJobCv_.notify_one();
}

void AppController::PrepareFreshLuaState(sol::state& state) {
    InitLuaCore(state);
    // The shutdown-watchdog hook in RunAutomationJob resolves `__smatchet_app_ui`
    // as AppController* to read `shuttingDown_`. InitLuaUi is intentionally not run
    // on these off-UI-thread states (no ImGui surface) — UI-mutating bindings stay
    // no-ops here (see smatchet::lua::InitLuaCore) — so set the UI alias directly.
    state["__smatchet_app_ui"] = this;
}

void AppController::ReplayActiveSetupScripts(sol::state& state, sol::environment& sandbox) {
    // Snapshot activeSetupScripts_ under the same mutex used by RunLuaSetupScript so the
    // iteration below sees a stable view even if the UI thread mutates the vector mid-job.
    std::vector<std::string> setupScriptsSnapshot;
    {
        std::lock_guard<std::mutex> lock(automationJobMutex_);
        setupScriptsSnapshot = activeSetupScripts_;
    }

    // Load and run setup scripts so global actions / mcp.register_tool definitions are
    // present for this job.
    //
    // Lifecycle contract (backlog #34): callers use a *fresh* sol::state per job/call for
    // isolation, so every top-level statement in a setup script re-fires on every job.
    // Setup scripts MUST therefore be defining-only — declaring functions, tables, constants —
    // and avoid side effects at module-load (no `tracker.create_issue(...)` at the top level,
    // no `os.execute(...)` outside of a function body, etc.). Wrap any such side-effecting work
    // in a function that the job explicitly invokes, and the re-execution becomes harmless.
    // Caching compiled sol::function refs across jobs would not change this: bytecode bound to
    // a destroyed state cannot be replayed, and persisting the state across jobs would lose the
    // isolation guarantee.
    for (const auto& path : setupScriptsSnapshot) {
        std::string resolved = ResolveLuaScriptPath(path);
        if (resolved.empty()) {
            continue;
        }
        auto script = state.load_file(resolved);
        if (!script.valid()) {
            sol::error err = script;
            const std::string bare = "[LUA setup-bg] " + path + ": " + err.what();
            LuaLogInfoBind(std::string("[ERROR] ") + bare);
            for (const auto& sink : errorSinks_) {
                sink(bare);
            }
            scriptingWindowOpenRequested_.store(true);
            continue;
        }
        sol::protected_function func = script;
        sandbox.set_on(func);
        sol::protected_function_result res = func();
        if (!res.valid()) {
            sol::error err = res;
            const std::string bare = "[LUA setup-bg] " + path + ": " + err.what();
            LuaLogInfoBind(std::string("[ERROR] ") + bare);
            for (const auto& sink : errorSinks_) {
                sink(bare);
            }
            scriptingWindowOpenRequested_.store(true);
        }
    }
}

void AppController::AutomationWorkerLoop() {
    // Per-iteration try/catch wrapping all sol2/JSON/STL paths. Without this, a single throw
    // (sol::error from a malformed script, std::bad_alloc from a runaway capture, etc.) escapes
    // the thread function and triggers std::terminate. The error is logged and the worker
    // continues serving the next job — same liveness contract as a UI-thread exception handler.
    while (true) {
        AutomationJob job;
        {
            std::unique_lock<std::mutex> lock(automationJobMutex_);
            automationJobCv_.wait(lock, [this]() {
                return shuttingDown_.load() || automationWorkerShuttingDown_.load() || !automationJobs_.empty();
            });
            if (shuttingDown_.load() || automationWorkerShuttingDown_.load()) {
                break;
            }
            job = std::move(automationJobs_.front());
            automationJobs_.pop_front();
        }

        try {
            // Fresh per-job sol::state for isolation — see PrepareFreshLuaState /
            // ReplayActiveSetupScripts. Same pattern the MCP run_lua / tool handlers use.
            sol::state bgState;
            PrepareFreshLuaState(bgState);
            sol::environment sandbox = CreateSandboxEnvironment(bgState);
            ReplayActiveSetupScripts(bgState, sandbox);
            RunAutomationJob(bgState, sandbox, job);
        } catch (const std::exception& ex) {
            LOG_ERROR("AppController::AutomationWorkerLoop: exception escaped job '%s': %s",
                      job.scriptPathOrActionName.c_str(), ex.what());
        } catch (...) {
            LOG_ERROR("AppController::AutomationWorkerLoop: unknown exception escaped job '%s'",
                      job.scriptPathOrActionName.c_str());
        }
    }
}

void AppController::RunAutomationJob(sol::state& state, sol::environment& env, const AutomationJob& job) {
    FieldEditAuditSource::ScopedOverride luaSource(FieldEditAuditSource::kLua);

    lua_sethook(
        state.lua_state(),
        [](lua_State* L, lua_Debug* /*ar*/) {
            sol::state_view sv(L);
            // `__smatchet_app_ui` is the AppController* alias (see ResolveApp comment).
            // The Core `__smatchet_app` now holds an `ILuaBindingHost*`; resolving it
            // as AppController* would corrupt under multiple inheritance.
            const sol::object appObj = sv["__smatchet_app_ui"];
            AppController* app = nullptr;
            if (appObj.valid() && appObj.get_type() != sol::type::lua_nil) {
                app = appObj.as<AppController*>();
            }
            if (app && app->shuttingDown_.load()) {
                luaL_error(L, "Script execution aborted (shutdown).");
            }
        },
        LUA_MASKCOUNT, 50000);

    auto logErr = [this](const char* prefix, const std::string& detail) {
        const std::string bare = std::string(prefix) + detail;
        // Route through the normal info sink so the console shows it, but also
        // through dedicated error sinks (persistent error panel + window-open).
        LuaLogInfoBind(std::string("[ERROR] ") + bare);
        for (const auto& sink : errorSinks_) {
            sink(bare);
        }
        scriptingWindowOpenRequested_.store(true);
    };

    if (job.type == AutomationJob::Type::RunAutoScript) {
        const std::string path = ResolveLuaScriptPath(job.scriptPathOrActionName);
        if (path.empty()) {
            logErr("[LUA auto] ", "invalid script path");
            return;
        }

        sol::load_result script = state.load_file(path);
        if (!script.valid()) {
            sol::error err = script;
            logErr("[LUA auto] ", err.what());
            return;
        }

        sol::protected_function func = script;
        sol::environment runEnv = CreateSandboxEnvironment(state);
        runEnv.set_on(func);

        sol::protected_function_result init_pfr = func();
        if (!init_pfr.valid()) {
            sol::error err = init_pfr;
            logErr("[LUA auto] ", err.what());
            return;
        }

        sol::protected_function process_func = runEnv["process_ticket"];
        if (!process_func.valid()) {
            logErr("[LUA auto] ", "script must define function process_ticket(ticket)");
            return;
        }

        const auto snap = GetActiveTicketsSnapshot();
        std::unordered_set<std::string> selectedSet(job.selectedIds.begin(), job.selectedIds.end());

        for (auto& ticket : *snap) {
            if (!selectedSet.empty() && selectedSet.find(ticket.id) == selectedSet.end()) {
                continue;
            }
            if (selectedSet.empty()) {
                break;
            }

            // Copy ticket so we don't modify the snapshot elements in-place directly without protection
            CachedTicket ticketCopy = ticket;
            sol::protected_function_result pfr = process_func(&ticketCopy);
            if (!pfr.valid()) {
                sol::error err = pfr;
                logErr("[LUA auto] ", err.what());
            }
        }
    } else if (job.type == AutomationJob::Type::TicketAction) {
        sol::protected_function func = env[job.scriptPathOrActionName];
        if (func.valid()) {
            sol::protected_function_result pfr = func(job.targetIssueId);
            if (!pfr.valid()) {
                sol::error err = pfr;
                logErr("[LUA action] ", err.what());
            }
        } else {
            logErr("[LUA action] ", "Function not found: " + job.scriptPathOrActionName);
        }
    } else if (job.type == AutomationJob::Type::GlobalAction) {
        sol::protected_function func = env[job.scriptPathOrActionName];
        if (func.valid()) {
            sol::protected_function_result pfr = func();
            if (!pfr.valid()) {
                sol::error err = pfr;
                logErr("[LUA action] ", err.what());
            }
        } else {
            logErr("[LUA action] ", "Function not found: " + job.scriptPathOrActionName);
        }
    } else if (job.type == AutomationJob::Type::RunFlatScript) {
        const std::string path = ResolveLuaScriptPath(job.scriptPathOrActionName);
        if (path.empty()) {
            logErr("[LUA run] ", "invalid script path");
            return;
        }

        sol::load_result script = state.load_file(path);
        if (!script.valid()) {
            sol::error err = script;
            logErr("[LUA run] ", err.what());
            return;
        }

        sol::protected_function func = script;
        sol::environment runEnv = CreateSandboxEnvironment(state);
        runEnv.set_on(func);

        sol::protected_function_result pfr = func();
        if (!pfr.valid()) {
            sol::error err = pfr;
            logErr("[LUA run] ", err.what());
        } else {
            LOG_TRACE("RunAutomationJob: flat script finished.");
        }
    }

    lua_sethook(state.lua_state(), nullptr, 0, 0);
}

void AppController::RunLuaSetupScript(const std::string& scriptPath) {
    auto logErr = [this](const char* prefix, const std::string& detail) {
        const std::string bare = std::string(prefix) + detail;
        const std::string decorated = std::string("[ERROR] ") + bare;
        if (luaHost_ && !luaHost_->SnapshotLogSinks().empty()) {
            for (const auto& sink : luaHost_->SnapshotLogSinks()) {
                sink(decorated);
            }
        } else {
            LOG_ERROR("%s", decorated.c_str());
        }
        for (const auto& sink : errorSinks_) {
            sink(bare);
        }
        scriptingWindowOpenRequested_.store(true);
    };

    const std::string path = ResolveLuaScriptPath(scriptPath);
    if (path.empty()) {
        logErr("[LUA setup] ", "invalid script path");
        return;
    }

    // activeSetupScripts_ is read by AutomationWorkerLoop on the worker thread — every mutation
    // must take automationJobMutex_ so the worker's snapshot copy sees a consistent vector.
    {
        std::lock_guard<std::mutex> lock(automationJobMutex_);
        if (std::find(activeSetupScripts_.begin(), activeSetupScripts_.end(), scriptPath) ==
            activeSetupScripts_.end()) {
            activeSetupScripts_.push_back(scriptPath);
        }
    }

    LOG_TRACE("RunLuaSetupScript: begin path=%s scriptPath=%s", path.c_str(), scriptPath.c_str());
    FieldEditAuditSource::ScopedOverride luaSource(FieldEditAuditSource::kLua);

    sol::environment sandbox = CreateSandboxEnvironment(lua);
    sol::load_result script = lua.load_file(path);
    if (!script.valid()) {
        sol::error err = script;
        logErr("[LUA setup] ", err.what());
        LOG_TRACE("RunLuaSetupScript: load_error path=%s %s", path.c_str(), err.what());
        return;
    }
    sol::protected_function func = script;
    sandbox.set_on(func);

    lua_sethook(
        lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) { luaL_error(L, "Script execution timeout exceeded."); },
        LUA_MASKCOUNT, 100000);

    auto res = func();

    lua_sethook(lua.lua_state(), nullptr, 0, 0);

    if (!res.valid()) {
        sol::error err = res;
        logErr("[LUA setup] ", err.what());
        LOG_TRACE("RunLuaSetupScript: failed path=%s", path.c_str());
    } else {
        LOG_TRACE("RunLuaSetupScript: ok path=%s", path.c_str());
    }
}
