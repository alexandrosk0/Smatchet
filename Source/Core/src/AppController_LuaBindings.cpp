#include "AppController.h"
#include "AppControllerImpl.h" // AppController::Impl — cold sol2/automation member storage (pImpl #19b)
#include "ILuaBindingHost.h"
#include "LuaAutomationHost.h"
#include "LocalCacheManager.h" // direct: AppController.h now fwd-decls LocalCacheManager (fan-in Phase 1); this TU calls app_.Cache-> methods.

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "AiAssistantController.h"
#include "AiLuaPromptRateLimit.h"
#include "ConfigManager.h"
#include "FieldEditAuditSource.h"
#include "IssueTableSerializer.h"
#include "LuaAutomationHookPolicyPure.h"
#include "SmatchetToast.h"

#include <algorithm>
#include <atomic>
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

#include "Json/BoundedJsonParse.h"

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

// JSON <-> Lua marshalling moved to the shared Json/LuaJsonConvert.h leaf
// (reached via AppController_LuaBindings_detail.h). The public JsonToLua /
// LuaToJson are global-scope inline there; call sites below are unchanged.
// decode_json's threat-model rationale now lives above LuaDecodeJsonBind.

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

// CPP_CODE_AUDIT.md #20: dense-array join recursion depth cap, mirroring
// lua_json_detail::kJsonToLuaMaxDepth — without it a cyclic dense array
// (`local t={}; t[1]=t`) recurses without bound (stack overflow, SIGSEGV).
constexpr int kLuaObjectToIssueFieldStringMaxDepth = 64;

/** Flatten Lua values the same way as JSON import cells (strings, numbers, bools, simple arrays). */
static std::string LuaObjectToIssueFieldString(const sol::object& v, std::size_t maxDump = 4096, int depth = 0) {
    if (depth > kLuaObjectToIssueFieldStringMaxDepth) {
        return std::string("?");
    }
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
                    joined += LuaObjectToIssueFieldString(tbl[i], maxDump / (maxIdx + 1), depth + 1);
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
static AppController::Impl* ResolveApp(sol::this_state L) {
    sol::state_view lua(L);
    const sol::object appObj = lua["__smatchet_app_ui"];
    if (!appObj.valid() || appObj.get_type() == sol::type::lua_nil) {
        return nullptr;
    }
    return appObj.as<AppController::Impl*>();
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
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaRegisterFieldDisplayCachedBind(fieldId, std::move(fn));
}

void LuaUnregisterFieldDisplayCachedGlue(sol::this_state L, const std::string& fieldId) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUnregisterFieldDisplayCachedBind(fieldId);
}

void LuaRegisterFieldDisplayCachedByNameGlue(sol::this_state L, const std::string& displayName, sol::function fn) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaRegisterFieldDisplayCachedByNameBind(displayName, std::move(fn));
}

void LuaUnregisterFieldDisplayCachedByNameGlue(sol::this_state L, const std::string& displayName) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUnregisterFieldDisplayCachedByNameBind(displayName);
}

void LuaUiInvalidateWindowGlue(sol::this_state L, const std::string& name) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->app_.LuaUiInvalidateWindowBind(name);
}

void LuaUiInvalidateFieldCacheGlue(sol::this_state L, sol::optional<std::string> ticketId,
                                   sol::optional<std::string> fieldId) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUiInvalidateFieldCacheBind(ticketId, fieldId);
}

void LuaUiUnregisterWindowGlue(sol::this_state L, const std::string& name) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUiUnregisterWindowBind(name);
}

void LuaRegisterFieldIconMapGlue(sol::this_state L, const std::string& fieldKey, sol::table map,
                                 sol::optional<bool> byName) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaRegisterFieldIconMapBind(fieldKey, std::move(map), byName);
}

void LuaUnregisterFieldIconMapGlue(sol::this_state L, const std::string& fieldKey, sol::optional<bool> byName) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUnregisterFieldIconMapBind(fieldKey, byName);
}

void LuaImGuiTextGlue(sol::this_state L, const std::string& s) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return;
    }
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaImGuiTextBind(s);
}

void LuaImGuiTextUnformattedGlue(sol::this_state L, const std::string& s) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return;
    }
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaImGuiTextUnformattedBind(s);
}

bool LuaImGuiImageGlue(sol::this_state L, const std::string& path, float w, float h) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return false;
    }
    AppController::Impl* app = ResolveApp(L);
    return app ? app->LuaImGuiImageBind(path, w, h) : false;
}

void LuaUiRegisterWindowGlue(sol::this_state L, const std::string& name, sol::function drawFn) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUiRegisterWindowBind(name, std::move(drawFn));
}

void LuaUiRegisterTicketActionGlue(sol::this_state L, const std::string& name, const std::string& cb) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUiRegisterTicketActionBind(name, cb);
}

void LuaUiRegisterGlobalActionGlue(sol::this_state L, const std::string& name, const std::string& cb) {
    AppController::Impl* app = ResolveApp(L);
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
    AppController::Impl* app = ResolveApp(L);
    if (!app)
        return;
    app->app_.AddAiContext(LuaTableToAiContextBlock(blockTbl));
}

void LuaAiClearContextGlue(sol::this_state L) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->app_.ClearAiContext();
}

void LuaAiPromptGlue(sol::this_state L, const std::string& prompt, sol::optional<sol::table> extraBlocks) {
    AppController::Impl* app = ResolveApp(L);
    if (!app)
        return;
    // Rate-limit + consent gate (security audit H5 / E6). The instruction-count
    // lua_sethook does NOT cover the outbound HTTP this kicks off, so reject a
    // re-entrant or <5 s-spaced call BEFORE any context mutation / submit, and
    // fire the one-time consent toast on the first accepted call. luaL_error
    // raises a Lua error (caught by the protected call) rather than blocking the
    // UI thread — no sleep/spin.
    std::string gateError;
    if (!app->TryBeginLuaAiPromptTurn(gateError)) {
        luaL_error(L, "%s", gateError.c_str());
        return;
    }
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
            app->app_.AddAiContext(LuaTableToAiContextBlock(el.as<sol::table>()));
        }
    }
    app->app_.PromptAi(prompt);
    // Submit() hands the turn to the AI worker thread; the synchronous glue work
    // is done, so release the in-flight slot. The 5 s spacing rule (stamped at
    // TryBegin) now guards the next call. Re-entrancy is still blocked for the
    // duration of THIS call (a context-builder that re-entered ai.prompt would
    // hit the in-flight reject above).
    app->EndLuaAiPromptTurn();
}

// LuaCommandsInvokeGlue / LuaMcpRegisterToolGlue / LuaCreateIssueGlue /
// LuaTrackerGetTypeGlue / LuaTrackerCreateIssueGlue lifted to
// AppController_LuaBindingsCore.cpp (ImGui-free TU).

} // namespace smatchet_lua_init_detail

void AppController::InitLua() {
    sol::state& lua = impl_->lua; // pImpl #19b: the sol::state member now lives in AppController::Impl
    impl_->InitLuaCore(lua);      // #19c: Init* relocated onto Impl
    impl_->InitLuaUi(lua);
}

// Lifted to `smatchet::lua::InitLuaCore(state, host)` in AppController_LuaBindingsCore.cpp.
// This forwarder keeps the existing call sites (`AppController::InitLua` + the
// AutomationWorkerLoop `InitLuaCore(bgState)`) source-compatible. The host
// pointer passed through is `this` (AppController inherits from ILuaBindingHost
// when SMATCHET_WITH_LUA_AUTOMATION is on).
void AppController::Impl::InitLuaCore(sol::state& state) { smatchet::lua::InitLuaCore(state, this); }

void AppController::Impl::InitLuaUi(sol::state& state) {
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

bool AppController::Impl::TryBeginLuaAiPromptTurn(std::string& outError) {
    std::lock_guard<std::mutex> lk(aiPromptGateMutex_);
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const std::int64_t lastMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(aiPromptLastCallAt_.time_since_epoch()).count();

    const smatchet::ai::AiPromptGateDecision decision =
        smatchet::ai::DecideAiPromptGate(aiPromptInFlight_, aiPromptEverCalled_, lastMs, nowMs);
    if (decision == smatchet::ai::AiPromptGateDecision::RejectReentrant) {
        outError = "ai.prompt rejected: a previous prompt is still in flight (re-entrant call blocked)";
        return false;
    }
    if (decision == smatchet::ai::AiPromptGateDecision::RejectTooSoon) {
        outError = "ai.prompt rejected: rate limit — wait at least 5 s between prompts";
        return false;
    }

    // Accepted — claim the in-flight slot + stamp the timestamp under the lock.
    aiPromptInFlight_ = true;
    aiPromptLastCallAt_ = now;
    aiPromptEverCalled_ = true;

    // One-time-per-session consent toast naming the outbound provider host, so a
    // user who pasted-and-ran a script knows ai.prompt just reached off-host.
    if (!aiPromptConsentShown_) {
        aiPromptConsentShown_ = true;
        std::string provider = "the configured AI provider";
#if defined(SMATCHET_WITH_AI)
        if (aiAssistant_) {
            const std::string name = aiAssistant_->GetActiveProviderName();
            if (!name.empty()) {
                provider = name;
            }
        }
#endif
        SmatchetToastManager::Instance().Push(
            "AI prompt from Lua", "A Lua script called ai.prompt — sending your AI context to " + provider + ".",
            ToastType::Warning, 8000);
        LOG_INFO("[LUA] ai.prompt invoked from Lua for the first time this session (provider=%s)", provider.c_str());
    }
    return true;
}

void AppController::Impl::EndLuaAiPromptTurn() {
    std::lock_guard<std::mutex> lk(aiPromptGateMutex_);
    aiPromptInFlight_ = false;
}

void AppController::Impl::LuaLogInfoBind(const std::string& msg) {
    const std::string clean = SanitizeLogText(msg);
    if (luaHost_ && !luaHost_->SnapshotLogSinks().empty()) {
        for (const auto& sink : luaHost_->SnapshotLogSinks()) {
            sink(clean);
        }
    } else {
        LOG_INFO("[LUA] %s", clean.c_str());
    }
}

// Sol-free interface methods kept on AppController (see AppController.h); Impl forwards to app_.
std::vector<CachedTicket> AppController::LuaGetActiveTicketsBind() {
    const auto snap = GetActiveTicketsSnapshot();
    return std::vector<CachedTicket>(snap->begin(), snap->end());
}
std::vector<CachedTicket> AppController::Impl::LuaGetActiveTicketsBind() { return app_.LuaGetActiveTicketsBind(); }
const TrackerField* AppController::Impl::FindFieldById(const std::string& fieldId) const {
    return app_.FindFieldById(fieldId);
}
bool AppController::Impl::SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                                          const std::vector<std::string>& rawValues, std::string& outError) {
    return app_.SubmitFieldEdit(issueId, field, rawValues, outError);
}
smatchet::cmd::CommandRegistry& AppController::Impl::LuaCommands() { return app_.Commands(); }
AppController* AppController::Impl::AppForCommandContext() { return &app_; }

std::tuple<sol::object, std::string> AppController::Impl::LuaGetTicketBind(sol::state_view sv,
                                                                           const std::string& issueId) {
    // Marshal against the *calling* state `sv`, not the member `lua`: the caller may be an
    // off-UI-thread fresh state (MCP / automation worker). Touching `lua` here would re-introduce
    // cross-thread lua_State access + a cross-state sol::object return. See
    // docs/plans/shipped/mcp-lua-fresh-state-race.md.
    CachedTicket ticket;
    // CacheBackendKeyCopy is mutex-guarded — this bind runs on the Lua automation / MCP
    // worker thread while the UI thread may re-stamp the key on a tracker swap (Slice 1b).
    if (app_.Cache->TryGetTicket(app_.focusedContext().CacheBackendKeyCopy(), issueId, ticket)) {
        return {sol::make_object(sv, ticket), ""};
    }
    return {sol::make_object(sv, sol::nil), "Ticket not found in local cache"};
}

// `decode_json` parses ATTACKER-CONTROLLED text (a Lua script can hand it any
// string). nlohmann's DOM parser is iterative (it drives `json_sax_dom_parser`
// from `sax_parse`, NOT a per-level recursion), so a deeply-nested payload
// ("[[[[...]]]]") does NOT overflow while parsing — it builds the full DOM, and
// THAT deep tree overflows the C++ stack when it is destroyed (`~json` recurses
// per nesting level) and grows the heap unboundedly while it is built (Pillar 3 —
// Never crash). The 4 MB byte cap in LuaDecodeJsonBind does NOT bound depth —
// ~2 M nested arrays fit easily.
// The depth/node-bounded parse lives in the shared smatchet::json_safe helper
// (Source/Core/include/Json/BoundedJsonParse.h) so this sink, the MCP REST /
// JSON-RPC ingress, and the Lua-MCP-tool params path all route through ONE
// BoundedDecodeSax (DRY / Pillar 5). The caps sit far above any legitimate JSON
// the bindings exchange. On overflow we return a parse error string up to Lua —
// graceful degradation, NOT a C++ throw across the sol2 boundary.
std::tuple<sol::object, std::string> AppController::Impl::LuaDecodeJsonBind(sol::state_view sv, const std::string& s) {
    // Marshal against the calling state `sv` (see LuaGetTicketBind).
    // Depth/node-bounded parse via the shared json_safe helper (NOT a bare
    // json::parse, which would build the full DOM and then stack-overflow when that
    // deep tree is torn down — see the recursive `~json` note above). On a cap hit
    // ParseBounded returns null + a non-empty errOut: report it to Lua as a parse
    // error string rather than crashing or throwing across the sol2 boundary
    // (Pillar 3). Keep the 4 MiB byte cap on top of the depth/node caps.
    constexpr std::size_t kMaxDecodeBytes = 4u * 1024u * 1024u;
    std::string err;
    const nlohmann::json j = smatchet::json_safe::ParseBounded(s, err, kMaxDecodeBytes);
    if (!err.empty()) {
        if (err == smatchet::json_safe::OverflowError()) {
            // decode_json is reachable concurrently (Lua automation + MCP worker
            // threads), so the once-only warn latch must be atomic — a plain `static
            // bool` is a data race under concurrent overflow (issue #1287). exchange()
            // makes exactly one thread observe the false→true edge.
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                LOG_WARN("decode_json: input exceeded depth (%d) or node (%zu) cap; rejecting. "
                         "Possible hostile or malformed payload.",
                         smatchet::json_safe::kDefaultMaxDepth, smatchet::json_safe::kDefaultMaxNodes);
            }
        }
        return {sol::make_object(sv, sol::nil), err};
    }
    return {JsonToLua(sv, j), std::string()};
}

std::tuple<sol::object, std::string> AppController::Impl::LuaCreateIssueBind(sol::state_view sv, sol::table spec) {
    // Marshal against the calling state `sv`, not the member `lua` (see LuaGetTicketBind):
    // `spec` is already on `sv`, and the result table must be too.
    const TrackerConfig cfg = ConfigManager::Load();
    // Same base as the grid new-issue row: config fallbacks plus last-row project / issue type when present.
    IssueDraft draft = app_.BuildDraftFromLastTicket(cfg);

    sol::object offlineObj = spec["offline"];
    if (!offlineObj.valid() || offlineObj.get_type() == sol::type::lua_nil) {
        offlineObj = spec["queue_offline"];
    }
    const bool offline = LuaTruthy(offlineObj);

    LuaMergeIssueCreateSpec(draft, std::move(spec), app_.fieldCatalog().AvailableFields);

    sol::table result = sv.create_table();

    if (offline) {
        if (!app_.Cache) {
            return {sol::make_object(sv, sol::nil),
                    std::string("Local cache not initialized (cannot queue offline create)")};
        }
        const std::int64_t qid = app_.QueueCreateOffline(draft);
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

    std::future<IssueCreateResult> fut = app_.CreateIssueAsync(draft);
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

void AppController::Impl::LuaRegisterFieldDisplayCachedBind(const std::string& fieldId, sol::function fn) {
    if (fieldId.empty() || !fn.valid()) {
        return;
    }
    fieldDisplayCachedProviders_[fieldId] = sol::protected_function(std::move(fn));
    // Bump invalidates every cached entry that holds a (possibly-stale) provider ref. Per-entry
    // input comparison in TryRenderCachedLuaField handles ordinary value changes; this is the
    // explicit "registration churn happened" channel.
    app_.luaProviderGen_.fetch_add(1);
}

void AppController::Impl::LuaUnregisterFieldDisplayCachedBind(const std::string& fieldId) {
    fieldDisplayCachedProviders_.erase(fieldId);
    app_.luaProviderGen_.fetch_add(1);
}

void AppController::Impl::LuaRegisterFieldDisplayCachedByNameBind(const std::string& displayName, sol::function fn) {
    if (displayName.empty() || !fn.valid()) {
        return;
    }
    fieldDisplayCachedProvidersByName_[AsciiLowerCopy(displayName)] = sol::protected_function(std::move(fn));
    app_.luaProviderGen_.fetch_add(1);
}

void AppController::Impl::LuaUnregisterFieldDisplayCachedByNameBind(const std::string& displayName) {
    fieldDisplayCachedProvidersByName_.erase(AsciiLowerCopy(displayName));
    app_.luaProviderGen_.fetch_add(1);
}

void AppController::Impl::LuaRegisterFieldIconMapBind(const std::string& fieldKey, sol::table map,
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

void AppController::Impl::LuaUnregisterFieldIconMapBind(const std::string& fieldKey, sol::optional<bool> byName) {
    std::lock_guard<std::mutex> lock(fieldIconMapsMutex_);
    if (byName.value_or(false)) {
        fieldIconMapsByDisplayName_.erase(ToLowerAsciiCopy(fieldKey));
    } else {
        fieldIconMapsByFieldId_.erase(fieldKey);
    }
}

void AppController::Impl::LuaImGuiTextBind(const std::string& s) { ImGui::TextUnformatted(s.c_str()); }

void AppController::Impl::LuaImGuiTextUnformattedBind(const std::string& s) { ImGui::TextUnformatted(s.c_str()); }

bool AppController::Impl::LuaImGuiImageBind(const std::string& path, float w, float h) {
    return SmatchetFieldIconRender::DrawImagePathOrUrl(app_, path, w, h);
}

void AppController::ApplyOrQueueLuaWindowOp(smatchet::lua::PendingLuaWindowOp op) {
    // UI-thread-only. See plan §Explicit window invalidation (F1).
    if (inDrawLuaWindows_) {
        impl_->pendingLuaWindowOps_.push_back(std::move(op));
        return;
    }
    switch (op.kind) {
    case smatchet::lua::PendingLuaWindowOp::Kind::Invalidate: {
        auto it = std::find_if(impl_->luaWindows_.begin(), impl_->luaWindows_.end(),
                               [&](const smatchet::lua::LuaWindowEntry& w) { return w.name == op.name; });
        if (it != impl_->luaWindows_.end()) {
            it->dirty = true;
            it->hasError = false;
            it->errorMessage.clear();
            return;
        }
        break;
    }
    case smatchet::lua::PendingLuaWindowOp::Kind::Register: {
        auto it = std::find_if(impl_->luaWindows_.begin(), impl_->luaWindows_.end(),
                               [&](const smatchet::lua::LuaWindowEntry& w) { return w.name == op.name; });
        if (it != impl_->luaWindows_.end())
            impl_->luaWindows_.erase(it);
        smatchet::lua::LuaWindowEntry e;
        e.name = op.name;
        e.drawFn = std::move(op.drawFn);
        e.dirty = true;
        impl_->luaWindows_.push_back(std::move(e));
        break;
    }
    case smatchet::lua::PendingLuaWindowOp::Kind::Unregister:
        impl_->luaWindows_.erase(
            std::remove_if(impl_->luaWindows_.begin(), impl_->luaWindows_.end(),
                           [&](const smatchet::lua::LuaWindowEntry& w) { return w.name == op.name; }),
            impl_->luaWindows_.end());
        break;
    }
}

void AppController::Impl::LuaUiRegisterWindowBind(const std::string& name, sol::function drawFn) {
    smatchet::lua::PendingLuaWindowOp op;
    op.kind = smatchet::lua::PendingLuaWindowOp::Kind::Register;
    op.name = name;
    if (drawFn.valid()) {
        op.drawFn = sol::protected_function(std::move(drawFn));
    }
    // F1 timing: on-UI uses the in-frame queue so a callback fired inside DrawLuaWindows
    // takes effect this frame, not the next. Off-UI hops the dispatcher first.
    if (app_.IsOnUiThread()) {
        app_.ApplyOrQueueLuaWindowOp(std::move(op));
    } else {
        AppController* self = &app_;
        app_.mainThreadDispatcher.PostToMainThread(
            [self, opCap = std::move(op)]() mutable { self->ApplyOrQueueLuaWindowOp(std::move(opCap)); });
    }
}

void AppController::Impl::LuaUiUnregisterWindowBind(const std::string& name) {
    smatchet::lua::PendingLuaWindowOp op;
    op.kind = smatchet::lua::PendingLuaWindowOp::Kind::Unregister;
    op.name = name;
    if (app_.IsOnUiThread()) {
        app_.ApplyOrQueueLuaWindowOp(std::move(op));
    } else {
        AppController* self = &app_;
        app_.mainThreadDispatcher.PostToMainThread(
            [self, opCap = std::move(op)]() mutable { self->ApplyOrQueueLuaWindowOp(std::move(opCap)); });
    }
}

void AppController::LuaUiInvalidateWindowBind(const std::string& name) {
    smatchet::lua::PendingLuaWindowOp op;
    op.kind = smatchet::lua::PendingLuaWindowOp::Kind::Invalidate;
    op.name = name;
    if (IsOnUiThread()) {
        ApplyOrQueueLuaWindowOp(std::move(op));
    } else {
        AppController* self = this;
        mainThreadDispatcher.PostToMainThread(
            [self, opCap = std::move(op)]() mutable { self->ApplyOrQueueLuaWindowOp(std::move(opCap)); });
    }
}

void AppController::Impl::LuaUiInvalidateFieldCacheBind(sol::optional<std::string> ticketId,
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
    if (app_.IsOnUiThread()) {
        apply();
    } else {
        app_.mainThreadDispatcher.PostToMainThread(std::move(apply));
    }
}

void AppController::NotifyLuaTicketDataChanged() { luaWindowDataGen_.fetch_add(1); }

void AppController::Impl::LuaUiRegisterTicketActionBind(const std::string& name, const std::string& callbackFuncName) {
    std::lock_guard<std::mutex> lock(luaActionsMutex_);
    luaTicketActions_.erase(
        std::remove_if(luaTicketActions_.begin(), luaTicketActions_.end(),
                       [&](const std::pair<std::string, std::string>& p) { return p.first == name; }),
        luaTicketActions_.end());
    if (!callbackFuncName.empty()) {
        luaTicketActions_.push_back({name, callbackFuncName});
    }
}

void AppController::Impl::LuaUiRegisterGlobalActionBind(const std::string& name, const std::string& callbackFuncName) {
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
    if (app_.commandRegistry_) {
        const std::string cmdName = "lua." + name;
        // De-dup: if the action was already registered (e.g. script reloaded), remove the old one
        // from the registry. There is no `Unregister` API (registrations are permanent for safety),
        // so we skip re-registration when the exact name is already present.
        if (!app_.commandRegistry_->HasExact(cmdName) && !name.empty() && !callbackFuncName.empty()) {
            smatchet::cmd::Command c;
            c.Name = cmdName;
            c.Category = "lua";
            c.Summary = "(Lua) " + name;
            c.Description = "Lua global action registered via ui.register_global_action(\"" + name + "\", ...).";
            c.Idempotent = false; // Lua actions may mutate state
            c.AsyncSafe = true;
            // Capture by value so the handler owns a copy of the callback name string.
            const std::string cbName = callbackFuncName;
            AppController* appPtr = &app_;
            c.Handler = [appPtr, cbName](const nlohmann::json& /*args*/, const smatchet::cmd::CommandContext& /*ctx*/) {
                appPtr->ExecuteLuaGlobalAction(cbName);
                return smatchet::cmd::CommandResult::Success(nlohmann::json::object());
            };
            try {
                app_.commandRegistry_->Register(std::move(c));
            } catch (const std::exception& ex) {
                LOG_WARN("LuaUiRegisterGlobalActionBind: could not register '%s' in registry: %s", cmdName.c_str(),
                         ex.what());
            }
        }
    }
}

void AppController::Impl::ParseMcpToolDef(const sol::table& toolDef, smatchet::lua::McpToolDefinition& out) {
    out.name = toolDef.get_or<std::string>("name", "");
    out.description = toolDef.get_or<std::string>("description", "");

    sol::object params = toolDef["parameters"];
    if (params.is<sol::table>()) {
        out.parametersSchema = LuaToJson(params);
    } else {
        std::string schemaStr = toolDef.get_or<std::string>("parameters_json", "{}");
        // Route through the shared depth/node-bounded parser, NOT a bare
        // nlohmann::json::parse (issue #1287). The parser builds iteratively, so a
        // deeply-nested schema string does not overflow while parsing — but the
        // resulting deep DOM stack-overflows on destruction (`~json` recurses), and a
        // try/catch around json::parse could NOT trap that overflow. ParseBounded
        // aborts the bounded SAX build before such a DOM exists, degrading to an empty
        // schema instead of crashing. schemaStr is same-user input (a setup .lua tool
        // def), so this is defense-in-depth, not a remote-reachable sink.
        // Default 4 MiB byte cap (json_safe::ParseBounded) is intentionally accepted here —
        // it mirrors kMaxDecodeBytes at the decode_json site, and a tool-def schema string is
        // never anywhere near that large.
        std::string parseErr;
        nlohmann::json parsed = smatchet::json_safe::ParseBounded(schemaStr, parseErr);
        if (parseErr.empty()) {
            out.parametersSchema = std::move(parsed);
        } else {
            LOG_DEBUG("Lua MCP tool: parameters_json rejected (%s); using empty schema", parseErr.c_str());
            out.parametersSchema = nlohmann::json::object();
        }
    }
}

void AppController::Impl::LuaMcpRegisterToolBind(sol::table toolDef, sol::function callback) {
    if (!toolDef.valid() || !callback.valid()) {
        return;
    }
    smatchet::lua::McpToolDefinition def;
    ParseMcpToolDef(toolDef, def);
    def.callback = sol::protected_function(std::move(callback));

    std::lock_guard<std::mutex> lock(luaMcpToolsMutex_);
    luaMcpTools_.erase(std::remove_if(luaMcpTools_.begin(), luaMcpTools_.end(),
                                      [&](const smatchet::lua::McpToolDefinition& d) { return d.name == def.name; }),
                       luaMcpTools_.end());

    luaMcpTools_.push_back(std::move(def));
}

void AppController::ClearLuaTicketContextGlue() {
    // Clear every container that holds sol::protected_function refs BEFORE nulling the
    // __smatchet_app pointer. RAII reverse-declaration destruction already destroys these
    // containers before `lua` (member-order invariant inside AppController::Impl), but
    // belt-and-suspenders: explicitly drop the handles here so a future re-ordering can't
    // turn this into a UAF. See plan §Shutdown ordering.
    impl_->luaFieldCache_.clear();
    impl_->fieldDisplayCachedProviders_.clear();
    impl_->fieldDisplayCachedProvidersByName_.clear();
    impl_->luaWindows_.clear();
    impl_->pendingLuaWindowOps_.clear();
    impl_->lua["__smatchet_app"] = sol::lua_nil;
    impl_->lua["__smatchet_app_ui"] = sol::lua_nil;
}

void AppController::RunAutoScript(const std::string& scriptPath, const std::vector<std::string>& selectedIds,
                                  bool processAll) {
    std::lock_guard<std::mutex> lock(impl_->automationJobMutex_);
    impl_->automationJobs_.push_back({AutomationJob::Type::RunAutoScript, scriptPath, selectedIds, "", processAll});
    impl_->automationJobCv_.notify_one();
}

void AppController::RunFlatScriptAsync(const std::string& scriptPath) {
    std::lock_guard<std::mutex> lock(impl_->automationJobMutex_);
    impl_->automationJobs_.push_back({AutomationJob::Type::RunFlatScript, scriptPath, {}, ""});
    impl_->automationJobCv_.notify_one();
}

void AppController::Impl::PrepareFreshLuaState(sol::state& state) {
    InitLuaCore(state);
    // The shutdown-watchdog hook in RunAutomationJob resolves `__smatchet_app_ui`
    // as AppController::Impl* to read `app_.shuttingDown_`. InitLuaUi is intentionally not run
    // on these off-UI-thread states (no ImGui surface) — UI-mutating bindings stay
    // no-ops here (see smatchet::lua::InitLuaCore) — so set the UI alias directly.
    state["__smatchet_app_ui"] = this;
}

void AppController::Impl::ReplayActiveSetupScripts(sol::state& state, sol::environment& sandbox) {
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
        std::string resolved = app_.ResolveLuaScriptPath(path);
        if (resolved.empty()) {
            continue;
        }
        auto script = state.load_file(resolved);
        if (!script.valid()) {
            sol::error err = script;
            const std::string bare = "[LUA setup-bg] " + path + ": " + err.what();
            LuaLogInfoBind(std::string("[ERROR] ") + bare);
            for (const auto& sink : app_.errorSinks_) {
                sink(bare);
            }
            app_.scriptingWindowOpenRequested_.store(true);
            continue;
        }
        sol::protected_function func = script;
        sandbox.set_on(func);
        sol::protected_function_result res = func();
        if (!res.valid()) {
            sol::error err = res;
            const std::string bare = "[LUA setup-bg] " + path + ": " + err.what();
            LuaLogInfoBind(std::string("[ERROR] ") + bare);
            for (const auto& sink : app_.errorSinks_) {
                sink(bare);
            }
            app_.scriptingWindowOpenRequested_.store(true);
        }
    }
}

void AppController::Impl::AutomationWorkerLoop() {
    // Per-iteration try/catch wrapping all sol2/JSON/STL paths. Without this, a single throw
    // (sol::error from a malformed script, std::bad_alloc from a runaway capture, etc.) escapes
    // the thread function and triggers std::terminate. The error is logged and the worker
    // continues serving the next job — same liveness contract as a UI-thread exception handler.
    while (true) {
        AppController::AutomationJob job;
        {
            std::unique_lock<std::mutex> lock(automationJobMutex_);
            automationJobCv_.wait(lock, [this]() {
                return app_.shuttingDown_.load() || automationWorkerShuttingDown_.load() || !automationJobs_.empty();
            });
            if (app_.shuttingDown_.load() || automationWorkerShuttingDown_.load()) {
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
    // Signal a clean loop exit so the dtor's bounded shutdown wait can distinguish
    // "worker finished" from "worker still stuck in blocking glue" (see
    // automationWorkerExited_ in AppControllerImpl.h). Set BEFORE the thread
    // function returns; the subsequent join in the dtor is the happens-before
    // barrier, so a relaxed-visible store here is observed there.
    automationWorkerExited_.store(true);
    automationJobCv_.notify_all();
}

// Automation count-hook tuning (security finding #13). The hook fires every kHookCountInterval
// Lua instructions; kAutomationInstructionBudget caps a single automation job so a runaway pure-Lua
// loop self-aborts even with no shutdown signal (exposure B). Budget chosen to allow substantial
// automation (~5e8 instructions ≈ 10000 hook ticks) while still terminating a true infinite loop.
// The abort/keep-running decision is the pure LuaAutomationHookPolicyPure predicate (unit-tested).
namespace {
constexpr int kHookCountInterval = 50000;
constexpr unsigned long long kAutomationInstructionBudget = 500000000ULL;

// Per-job accumulated instruction count. The worker is single-threaded and runs one job at a
// time on a fresh sol::state; reset in RunAutomationJob before each job. thread_local (not a
// member) so the count-hook — a plain C function pointer with no closure — can reach it.
thread_local unsigned long long g_automationInstructionsElapsed = 0ULL;
} // namespace

void AppController::Impl::RunAutomationJob(sol::state& state, sol::environment& env,
                                           const AppController::AutomationJob& job) {
    FieldEditAuditSource::ScopedOverride luaSource(FieldEditAuditSource::kLua);

    g_automationInstructionsElapsed = 0ULL;
    lua_sethook(
        state.lua_state(),
        [](lua_State* L, lua_Debug* /*ar*/) {
            g_automationInstructionsElapsed += static_cast<unsigned long long>(kHookCountInterval);
            sol::state_view sv(L);
            // `__smatchet_app_ui` is the AppController::Impl* alias (see ResolveApp comment).
            // The Core `__smatchet_app` now holds an `ILuaBindingHost*`; resolving it
            // as Impl* would corrupt under multiple inheritance.
            const sol::object appObj = sv["__smatchet_app_ui"];
            AppController::Impl* app = nullptr;
            if (appObj.valid() && appObj.get_type() != sol::type::lua_nil) {
                app = appObj.as<AppController::Impl*>();
            }
            // Shutdown abort must observe automationWorkerShuttingDown_ — it is raised BEFORE the
            // dtor's blocking automationWorker_.join(), whereas shuttingDown_ is set only AFTER the
            // join, so checking shuttingDown_ alone never released a long job during exit (#13 A).
            const bool shuttingDown = app && app->app_.shuttingDown_.load();
            const bool workerShuttingDown = app && app->automationWorkerShuttingDown_.load();
            const LuaAutomationHookPolicyPure::AbortReason reason = LuaAutomationHookPolicyPure::DecideAutomationAbort(
                shuttingDown, workerShuttingDown, g_automationInstructionsElapsed, kAutomationInstructionBudget);
            if (reason != LuaAutomationHookPolicyPure::AbortReason::kNone) {
                luaL_error(L, "%s", LuaAutomationHookPolicyPure::AbortReasonMessage(reason));
            }
        },
        LUA_MASKCOUNT, kHookCountInterval);

    auto logErr = [this](const char* prefix, const std::string& detail) {
        const std::string bare = std::string(prefix) + detail;
        // Route through the normal info sink so the console shows it, but also
        // through dedicated error sinks (persistent error panel + window-open).
        LuaLogInfoBind(std::string("[ERROR] ") + bare);
        for (const auto& sink : app_.errorSinks_) {
            sink(bare);
        }
        app_.scriptingWindowOpenRequested_.store(true);
    };

    if (job.type == AutomationJob::Type::RunAutoScript) {
        RunAutomationAutoScript(state, job, logErr);
    } else if (job.type == AutomationJob::Type::TicketAction) {
        RunAutomationActionCall(env, job, true, logErr);
    } else if (job.type == AutomationJob::Type::GlobalAction) {
        RunAutomationActionCall(env, job, false, logErr);
    } else if (job.type == AutomationJob::Type::RunFlatScript) {
        RunAutomationFlatScript(state, job, logErr);
    }

    lua_sethook(state.lua_state(), nullptr, 0, 0);
}

void AppController::Impl::RunAutomationAutoScript(sol::state& state, const AppController::AutomationJob& job,
                                                  const AutomationErrorSink& logErr) {
    const std::string path = app_.ResolveLuaScriptPath(job.scriptPathOrActionName);
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

    const auto snap = app_.GetActiveTicketsSnapshot();
    std::unordered_set<std::string> selectedSet(job.selectedIds.begin(), job.selectedIds.end());

    // Issue #824: an empty selection must require explicit intent. Without process_all we refuse
    // to run — never a silent mass-modify, never a silent no-op. With process_all set, the
    // selection filter is bypassed and every loaded ticket is processed.
    if (selectedSet.empty() && !job.processAll) {
        logErr("[LUA auto] ", "empty selection and process_all not set — refusing to run; "
                              "pass process_all=true to run across all loaded tickets");
        return;
    }

    for (auto& ticket : *snap) {
        // processAll bypasses the selection filter and runs across every ticket in the snapshot.
        if (!job.processAll && selectedSet.find(ticket.id) == selectedSet.end()) {
            continue;
        }

        // Copy ticket so we don't modify the snapshot elements in-place directly without protection
        CachedTicket ticketCopy = ticket;
        sol::protected_function_result pfr = process_func(&ticketCopy);
        if (!pfr.valid()) {
            sol::error err = pfr;
            logErr("[LUA auto] ", err.what());
        }
    }
}

void AppController::Impl::RunAutomationActionCall(sol::environment& env, const AppController::AutomationJob& job,
                                                  bool passTargetId, const AutomationErrorSink& logErr) {
    sol::protected_function func = env[job.scriptPathOrActionName];
    if (!func.valid()) {
        logErr("[LUA action] ", "Function not found: " + job.scriptPathOrActionName);
        return;
    }
    sol::protected_function_result pfr = passTargetId ? func(job.targetIssueId) : func();
    if (!pfr.valid()) {
        sol::error err = pfr;
        logErr("[LUA action] ", err.what());
    }
}

void AppController::Impl::RunAutomationFlatScript(sol::state& state, const AppController::AutomationJob& job,
                                                  const AutomationErrorSink& logErr) {
    const std::string path = app_.ResolveLuaScriptPath(job.scriptPathOrActionName);
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

void AppController::RunLuaSetupScript(const std::string& scriptPath) {
    auto logErr = [this](const char* prefix, const std::string& detail) {
        const std::string bare = std::string(prefix) + detail;
        const std::string decorated = std::string("[ERROR] ") + bare;
        if (impl_->luaHost_ && !impl_->luaHost_->SnapshotLogSinks().empty()) {
            for (const auto& sink : impl_->luaHost_->SnapshotLogSinks()) {
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
        std::lock_guard<std::mutex> lock(impl_->automationJobMutex_);
        if (std::find(impl_->activeSetupScripts_.begin(), impl_->activeSetupScripts_.end(), scriptPath) ==
            impl_->activeSetupScripts_.end()) {
            impl_->activeSetupScripts_.push_back(scriptPath);
        }
    }

    LOG_TRACE("RunLuaSetupScript: begin path=%s scriptPath=%s", path.c_str(), scriptPath.c_str());
    FieldEditAuditSource::ScopedOverride luaSource(FieldEditAuditSource::kLua);

    sol::state& lua = impl_->lua; // pImpl #19b: the sol::state member now lives in AppController::Impl
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
