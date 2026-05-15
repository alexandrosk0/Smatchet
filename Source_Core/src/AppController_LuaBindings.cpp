#include "AppController.h"
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

namespace {

std::string TruncateForTrace(const std::string& s, std::size_t maxLen = 480) {
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

// Thread-local: false during cached cell / window recording. The imgui.* glue checks this
// and luaL_errors if Lua tries to draw immediate-mode UI while a cached recording is active —
// otherwise the script would draw once on cache miss and silently vanish on the next replay.
// Per docs/design/applied/lua-recorded-cmd-list.md decision #5 + finding #6.
thread_local bool g_luaImmediateModeAllowed = true;

struct LuaImmediateModeGuard {
    bool prev;
    explicit LuaImmediateModeGuard(bool allow) : prev(g_luaImmediateModeAllowed) {
        g_luaImmediateModeAllowed = allow;
    }
    ~LuaImmediateModeGuard() { g_luaImmediateModeAllowed = prev; }
    LuaImmediateModeGuard(const LuaImmediateModeGuard&) = delete;
    LuaImmediateModeGuard& operator=(const LuaImmediateModeGuard&) = delete;
};

// RAII Lua instruction-count hook. Replaces the paired manual lua_sethook(...) +
// lua_sethook(nullptr) calls; survives early returns + C++ exceptions. Uniform count = 100000
// per plan §LuaHookGuard (Q7) — the old per-frame TryLuaFieldDisplay used 10000, but the
// cached design pays Lua cost only once per cell per refresh so the tight budget is no longer
// protective, just restrictive.
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

constexpr const char* kImmediateModeErrorMsg =
    "imgui.* not allowed inside cached provider / window — use draw:* instead";

// Recorder-method input-bounds caps per plan §Crash-safety hardening.
constexpr std::size_t kRecorderStringCap   = 64u * 1024u;
constexpr std::size_t kRecorderLabelCap    = 256u;
constexpr int         kRecorderMaxLen      = 65535;
constexpr float       kRecorderSizeCap     = 8192.0f;
constexpr float       kRecorderOffsetCap   = 4096.0f;

inline float ClampFloat(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline bool IsFiniteF(float v) {
    return std::isfinite(v);
}

inline void TruncateInPlace(std::string& s, std::size_t cap) {
    if (s.size() > cap) s.resize(cap);
}

// Forward-declared in the anonymous namespace so the `state.new_usertype<LuaDrawList>(...)`
// call in AppController::InitLuaUi sees a complete type. Method bodies live below; they
// reference AppController::ImCmd (declared in AppController.h).
//
// Method signatures omit `sol::this_state` (sol2 v2.20.6 won't bind member-fn pointers with
// `sol::this_state` as the first parameter). Recorder errors are raised by throwing
// std::runtime_error; sol2 catches std::exception inside pcall and surfaces them to Lua.
class LuaDrawList {
public:
    LuaDrawList() = default;
    LuaDrawList(const LuaDrawList&)            = delete;
    LuaDrawList& operator=(const LuaDrawList&) = delete;
    LuaDrawList(LuaDrawList&&)                 = delete;
    LuaDrawList& operator=(LuaDrawList&&)      = delete;

    void Text(std::string s);
    void TextUnformatted(std::string s);
    void Image(std::string path, float w, float h);
    void ProgressBar(float frac, float w, float h, sol::optional<std::string> overlay);
    void SameLine(sol::optional<float> offset, sol::optional<float> spacing);
    void Separator();
    void Dummy(float w, float h);
    void PushColor(int idx, float r, float g, float b, float a);
    void PopColor(sol::optional<int> count);
    void SetTooltip(std::string s);
    void Button(std::string label, sol::protected_function onClick);
    void InputText(std::string label, std::string initial,
                   int maxLen, sol::protected_function onCommit);
    void OnDeactivated(sol::protected_function fn);
    void OnDeactivatedAfterEdit(sol::protected_function fn);

    void Deactivate() { active_ = false; }
    std::vector<AppController::ImCmd> Take() { return std::move(cmds_); }

private:
    void RequireActive(const char* method);
    AppController::ImCmd* LastInteractive();

    std::vector<AppController::ImCmd> cmds_;
    int  interactiveIndex_ = 0;
    bool active_           = true;
};

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

sol::object JsonToLua(sol::state_view luaView, const nlohmann::json& j) {
    return JsonToLuaImpl(luaView, j, 0);
}

nlohmann::json LuaToJsonImpl(sol::object obj, int depth) {
    if (depth > 64) return nullptr;
    if (obj.get_type() == sol::type::lua_nil) return nullptr;
    if (obj.is<bool>()) return obj.as<bool>();
    if (obj.is<double>()) return obj.as<double>();
    if (obj.is<std::string>()) return obj.as<std::string>();
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

nlohmann::json LuaToJson(sol::object obj) {
    return LuaToJsonImpl(obj, 0);
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

std::string AsciiLowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
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
        if (d == std::floor(d) && d >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
            d <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
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
        } catch (...) {
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
    block("io");        // file / process I/O
    block("package");   // module loader (could load shared libs)
    block("debug");     // bytecode / locals introspection
    // Bytecode dump is mostly inert without `load`, but strip as defense-in-depth.
    sandbox["string"] = lua.globals()["string"];     // shadow + then patch the local copy
    // Cannot mutate the shared global `string` table (would leak to non-sandboxed paths
    // and break Lua-internal users of string.dump). Build a per-sandbox copy with the
    // dangerous fns blocked. Cheap — string is a small table of function refs.
    sol::table stringSafe = lua.create_table();
    sol::table stringGlobal = lua.globals()["string"];
    if (stringGlobal.valid()) {
        for (auto& kv : stringGlobal) {
            const std::string key = kv.first.as<std::string>();
            if (key == "dump") continue;             // strip bytecode dumper
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

} // namespace

/** GCC + sol2: lambdas with the same signature can share one demangled metatable name → heap corruption in Lua.
 *  All InitLua callables here are plain functions with distinct symbols (plus scoped AppController* for Ticket glue). */
namespace smatchet_lua_init_detail {

static AppController* ResolveApp(sol::this_state L) {
    sol::state_view lua(L);
    return lua["__smatchet_app"].get_or<AppController*>(nullptr);
}

std::tuple<bool, std::string> TicketSetFieldGlue(sol::this_state L, CachedTicket& t, const std::string& fieldId,
                                                  const std::string& val) {
    AppController* app = ResolveApp(L);
    if (!app) {
        return {false, "AppController not available for Ticket:set_field"};
    }
    LOG_TRACE("Lua Ticket:set_field audit_source=%s issue=%s field=%s val_len=%zu", FieldEditAuditSource::Current(),
              t.id.c_str(), fieldId.c_str(), val.size());
    const TrackerField* fieldMeta = app->FindFieldById(fieldId);
    if (!fieldMeta) {
        return {false, "Field not found in tracker catalog: " + fieldId};
    }
    std::string err;
    std::vector<std::string> vals;
    if (!val.empty()) {
        vals.push_back(val);
    }
    const bool ok = app->SubmitFieldEdit(t.id, *fieldMeta, vals, err);
    return {ok, err};
}

std::tuple<bool, std::string> TicketTransitionGlue(sol::this_state L, CachedTicket& t,
                                                    const std::string& statusName) {
    AppController* app = ResolveApp(L);
    if (!app) {
        return {false, "AppController not available for Ticket:transition"};
    }
    LOG_TRACE("Lua Ticket:transition audit_source=%s issue=%s status=%s", FieldEditAuditSource::Current(), t.id.c_str(),
              statusName.c_str());
    const TrackerField* statusField = app->FindFieldById("status");
    if (!statusField) {
        return {false, "Tracker 'status' field meta not found"};
    }
    std::string err;
    const bool ok = app->SubmitFieldEdit(t.id, *statusField, {statusName}, err);
    return {ok, err};
}

// imgui.* glue: each entry rejects when called inside a cached provider / window recording.
// `g_luaImmediateModeAllowed` is flipped false by LuaImmediateModeGuard around the
// `TryRenderCachedLuaField` provider call and the `DrawLuaWindows` record path; only
// event-time callbacks (`register_ticket_action`, `register_global_action`, MCP tools)
// leave it true. luaL_error unwinds via C++ exceptions thanks to the Lua-as-C++ build mode.
void ImGuiSameLineGlue(sol::this_state L) {
    if (!g_luaImmediateModeAllowed) { luaL_error(L, kImmediateModeErrorMsg); return; }
    ImGui::SameLine();
}

void ImGuiSeparatorGlue(sol::this_state L) {
    if (!g_luaImmediateModeAllowed) { luaL_error(L, kImmediateModeErrorMsg); return; }
    ImGui::Separator();
}

void ImGuiProgressBarGlue(sol::this_state L, float fraction, float width, float height) {
    if (!g_luaImmediateModeAllowed) { luaL_error(L, kImmediateModeErrorMsg); return; }
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
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return std::make_tuple(0.0f, 0.0f);
    }
    const ImVec2 v = ImGui::GetContentRegionAvail();
    return std::make_tuple(v.x, v.y);
}

bool ImGuiButtonGlue(sol::this_state L, const std::string& label) {
    if (!g_luaImmediateModeAllowed) { luaL_error(L, kImmediateModeErrorMsg); return false; }
    return ImGui::Button(label.c_str());
}

void LuaLogInfoGlue(sol::this_state L, std::string msg) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaLogInfoBind(std::move(msg));
}

std::tuple<sol::object, std::string> LuaGetTicketGlue(sol::this_state L, const std::string& issueId) {
    AppController* app = ResolveApp(L);
    if (!app) return {sol::object(), "AppController not available for get_ticket"};
    return app->LuaGetTicketBind(issueId);
}

std::vector<CachedTicket> LuaGetActiveTicketsGlue(sol::this_state L) {
    AppController* app = ResolveApp(L);
    if (!app) return {};
    return app->LuaGetActiveTicketsBind();
}

std::tuple<sol::object, std::string> LuaDecodeJsonGlue(sol::this_state L, const std::string& s) {
    AppController* app = ResolveApp(L);
    if (!app) return {sol::object(), "AppController not available for decode_json"};
    return app->LuaDecodeJsonBind(s);
}

void LuaRegisterFieldDisplayCachedGlue(sol::this_state L, const std::string& fieldId, sol::function fn) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaRegisterFieldDisplayCachedBind(fieldId, std::move(fn));
}

void LuaUnregisterFieldDisplayCachedGlue(sol::this_state L, const std::string& fieldId) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaUnregisterFieldDisplayCachedBind(fieldId);
}

void LuaRegisterFieldDisplayCachedByNameGlue(sol::this_state L, const std::string& displayName, sol::function fn) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaRegisterFieldDisplayCachedByNameBind(displayName, std::move(fn));
}

void LuaUnregisterFieldDisplayCachedByNameGlue(sol::this_state L, const std::string& displayName) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaUnregisterFieldDisplayCachedByNameBind(displayName);
}

void LuaUiInvalidateWindowGlue(sol::this_state L, const std::string& name) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaUiInvalidateWindowBind(name);
}

void LuaUiInvalidateFieldCacheGlue(sol::this_state L,
                                   sol::optional<std::string> ticketId,
                                   sol::optional<std::string> fieldId) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaUiInvalidateFieldCacheBind(ticketId, fieldId);
}

void LuaUiUnregisterWindowGlue(sol::this_state L, const std::string& name) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaUiUnregisterWindowBind(name);
}

void LuaRegisterFieldIconMapGlue(sol::this_state L, const std::string& fieldKey, sol::table map,
                                  sol::optional<bool> byName) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaRegisterFieldIconMapBind(fieldKey, std::move(map), byName);
}

void LuaUnregisterFieldIconMapGlue(sol::this_state L, const std::string& fieldKey, sol::optional<bool> byName) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaUnregisterFieldIconMapBind(fieldKey, byName);
}

void LuaImGuiTextGlue(sol::this_state L, const std::string& s) {
    if (!g_luaImmediateModeAllowed) { luaL_error(L, kImmediateModeErrorMsg); return; }
    AppController* app = ResolveApp(L);
    if (app) app->LuaImGuiTextBind(s);
}

void LuaImGuiTextUnformattedGlue(sol::this_state L, const std::string& s) {
    if (!g_luaImmediateModeAllowed) { luaL_error(L, kImmediateModeErrorMsg); return; }
    AppController* app = ResolveApp(L);
    if (app) app->LuaImGuiTextUnformattedBind(s);
}

bool LuaImGuiImageGlue(sol::this_state L, const std::string& path, float w, float h) {
    if (!g_luaImmediateModeAllowed) { luaL_error(L, kImmediateModeErrorMsg); return false; }
    AppController* app = ResolveApp(L);
    return app ? app->LuaImGuiImageBind(path, w, h) : false;
}

void LuaUiRegisterWindowGlue(sol::this_state L, const std::string& name, sol::function drawFn) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaUiRegisterWindowBind(name, std::move(drawFn));
}

void LuaUiRegisterTicketActionGlue(sol::this_state L, const std::string& name, const std::string& cb) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaUiRegisterTicketActionBind(name, cb);
}

void LuaUiRegisterGlobalActionGlue(sol::this_state L, const std::string& name, const std::string& cb) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaUiRegisterGlobalActionBind(name, cb);
}

// commands.invoke — wraps AppController::Commands().Dispatch so Lua scripts
// can call any registered command and receive a plain table {ok, data, error}.
sol::table LuaCommandsInvokeGlue(sol::this_state L, const std::string& cmdName, sol::optional<sol::table> argsTable) {
    sol::state_view sv(L);
    sol::table result = sv.create_table();
    AppController* app = ResolveApp(L);
    if (!app) {
        result["ok"] = false;
        result["error"] = std::string("AppController not available");
        return result;
    }
    nlohmann::json args = (argsTable) ? LuaToJson(argsTable.value()) : nlohmann::json::object();
    smatchet::cmd::CommandContext ctx;
    ctx.App = app;
    ctx.Source = smatchet::cmd::CommandSource::Lua;
    smatchet::cmd::CommandResult cr = app->Commands().Dispatch(cmdName, args, ctx);
    result["ok"] = cr.Ok;
    if (cr.Ok) {
        result["data"] = JsonToLua(sv, cr.Data);
    } else {
        sol::table errTbl = sv.create_table();
        errTbl["code"] = std::string(smatchet::cmd::ErrorCodeString(cr.Error.Code));
        errTbl["message"] = cr.Error.Message;
        errTbl["hint"] = cr.Error.Hint;
        result["error"] = errTbl;
    }
    return result;
}

void LuaMcpRegisterToolGlue(sol::this_state L, sol::table toolDef, sol::function callback) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaMcpRegisterToolBind(std::move(toolDef), std::move(callback));
}

std::tuple<sol::object, std::string> LuaCreateIssueGlue(sol::this_state L, sol::table spec) {
    AppController* app = ResolveApp(L);
    if (!app) return {sol::object(), "AppController not available for create_issue"};
    return app->LuaCreateIssueBind(std::move(spec));
}

std::string LuaTrackerGetTypeGlue() {
    return TrimCopy(ConfigManager::Load().TrackerType);
}

std::tuple<std::string, std::string> LuaTrackerCreateIssueGlue(sol::this_state L, sol::table fields) {
    AppController* app = ResolveApp(L);
    if (!app) {
        return {std::string(), std::string("AppController not available for tracker.create_issue")};
    }
    std::tuple<sol::object, std::string> bindRet = app->LuaCreateIssueBind(std::move(fields));
    const std::string& preflightErr = std::get<1>(bindRet);
    const sol::object& resObj = std::get<0>(bindRet);
    if (!preflightErr.empty()) {
        return {std::string(), preflightErr};
    }
    if (!resObj.valid() || resObj.get_type() == sol::type::lua_nil) {
        return {std::string(), std::string("create_issue returned no result")};
    }
    if (!resObj.is<sol::table>()) {
        return {std::string(), std::string("create_issue returned unexpected type")};
    }
    sol::table r = resObj.as<sol::table>();
    const sol::object oko = r["ok"];
    const bool ok = oko.valid() && oko.is<bool>() && oko.as<bool>();
    std::string key;
    const sol::object keyo = r["issue_key"];
    if (keyo.valid() && keyo.is<std::string>()) {
        key = keyo.as<std::string>();
    }
    std::string err;
    const sol::object erro = r["error"];
    if (erro.valid() && erro.is<std::string>()) {
        err = erro.as<std::string>();
    }
    if (ok && !key.empty()) {
        return {std::move(key), std::string()};
    }
    if (!err.empty()) {
        return {std::string(), std::move(err)};
    }
    return {std::string(), std::string("create_issue failed (no issue_key)")};
}

} // namespace smatchet_lua_init_detail

void AppController::InitLua() {
    InitLuaCore(lua);
    InitLuaUi(lua);
}

void AppController::InitLuaCore(sol::state& state) {
    state.open_libraries(sol::lib::base);
    state.open_libraries(sol::lib::string);
    state.open_libraries(sol::lib::table);
    // math is common in cell providers (math.floor for percent formatting). Without it,
    // scripts hit "attempt to index a nil value (global 'math')" — non-obvious if you
    // assume a standard Lua env.
    state.open_libraries(sol::lib::math);
    // os: do NOT open the standard lib — it ships with `os.execute`, `os.remove`,
    // `os.rename`, `os.exit`, `os.getenv`, `os.setlocale`, `os.tmpname`, which let a
    // script shell out, delete arbitrary files, read process env, or terminate the host.
    // Expose only the safe time/date subset as a whitelist. A future Lua version
    // adding a new dangerous os.* fn won't silently leak in (blacklist would).
    sol::table osSafe = state.create_table();
    osSafe.set_function("time",      []() -> std::time_t { return std::time(nullptr); });
    osSafe.set_function("clock",     []() -> double      { return static_cast<double>(std::clock()) / CLOCKS_PER_SEC; });
    osSafe.set_function("difftime",  [](std::time_t a, std::time_t b) -> double { return std::difftime(a, b); });
    osSafe.set_function("date",      [](sol::optional<std::string> fmt, sol::optional<std::time_t> t) -> std::string {
        const std::time_t when = t.value_or(std::time(nullptr));
        const std::string format = fmt.value_or(std::string("%c"));
        std::tm tmbuf{};
#if defined(_WIN32)
        localtime_s(&tmbuf, &when);
#else
        localtime_r(&when, &tmbuf);
#endif
        char out[256] = {};
        std::strftime(out, sizeof(out), format.c_str(), &tmbuf);
        return std::string(out);
    });
    state["os"] = osSafe;

    // Store AppController* per-state so glue functions resolve it via sol::this_state without a
    // process-wide pointer. Each state (main or background) holds its own entry — no cross-state
    // races and no lifetime hazard:
    //   - For the main `lua` state: ClearLuaTicketContextGlue (called from ~AppController before
    //     the lua dtor) nils the entry, so userdata that outlives the controller resolves to null.
    //   - For per-iteration `bgState` in AutomationWorkerLoop: bgState is a stack-local destroyed
    //     at the end of each iteration. ~AppController joins automationWorker_ before any member
    //     destruction (see the automationWorker_ contract in AppController.h), so the worker can
    //     never observe a half-destroyed AppController through `__smatchet_app`.
    state["__smatchet_app"] = this;

    state.new_usertype<CachedTicket>("Ticket",
        "id", &CachedTicket::id,
        "get_field", &CachedTicket::GetFieldValue,
        "set_field", &smatchet_lua_init_detail::TicketSetFieldGlue,
        "transition", &smatchet_lua_init_detail::TicketTransitionGlue);

    sol::table smatchet = state.create_table();
    smatchet.set_function("get_ticket", &smatchet_lua_init_detail::LuaGetTicketGlue);
    smatchet.set_function("get_active_tickets", &smatchet_lua_init_detail::LuaGetActiveTicketsGlue);
    smatchet.set_function("create_issue", &smatchet_lua_init_detail::LuaCreateIssueGlue);
    state["smatchet"] = smatchet;

    sol::table tracker = state.create_table();
    tracker.set_function("get_type", &smatchet_lua_init_detail::LuaTrackerGetTypeGlue);
    tracker.set_function("create_issue", &smatchet_lua_init_detail::LuaTrackerCreateIssueGlue);
    state["tracker"] = tracker;

    state.set_function("log_info", &smatchet_lua_init_detail::LuaLogInfoGlue);
    state.set_function("decode_json", &smatchet_lua_init_detail::LuaDecodeJsonGlue);

    // UI-mutating bindings (cached renderers, icon maps, ui.* table) live in InitLuaUi for
    // the main state. Background automation states re-run setup scripts in this minimal
    // Core state to define helper functions/tables — so the UI registrations need to be
    // present as no-ops here, otherwise SmatchetHooks.lua throws "attempt to call nil"
    // every time the worker spins up. The registrations are intentionally inert: the bgState
    // is destroyed at the end of each job, so any registration on it would be discarded
    // regardless. The real registrations happen against the main `lua` state via OnEarlyInit.
    auto noop = []() {};
    state.set_function("register_field_display_cached",         noop);
    state.set_function("unregister_field_display_cached",       noop);
    state.set_function("register_field_display_cached_by_name", noop);
    state.set_function("unregister_field_display_cached_by_name", noop);
    state.set_function("register_field_icon_map",               noop);
    state.set_function("unregister_field_icon_map",             noop);
    sol::table uiNoop = state.create_table();
    uiNoop.set_function("register_window",            noop);
    uiNoop.set_function("unregister_window",          noop);
    uiNoop.set_function("invalidate_window",          noop);
    uiNoop.set_function("invalidate_field_cache",     noop);
    uiNoop.set_function("invalidate_field_cache_for", noop);
    uiNoop.set_function("register_ticket_action",     noop);
    uiNoop.set_function("register_global_action",     noop);
    state["ui"] = uiNoop;

    sol::table mcp = state.create_table();
    mcp.set_function("register_tool", &smatchet_lua_init_detail::LuaMcpRegisterToolGlue);
    state["mcp"] = mcp;

    // Unified Command System: `commands.invoke("name", {arg=val})` → `{ok, data, error}`.
    // Lets Lua scripts call any registered command without needing direct AppController access.
    sol::table commands = state.create_table();
    commands.set_function("invoke", &smatchet_lua_init_detail::LuaCommandsInvokeGlue);
    state["commands"] = commands;
}

void AppController::InitLuaUi(sol::state& state) {
    // Cached-cmd-list cell renderer. Provider receives a 7th arg `draw` — a recorder; the
    // returned recording replays every frame until cache-key inputs change. See plan §Cells.
    state.set_function("register_field_display_cached",
                       &smatchet_lua_init_detail::LuaRegisterFieldDisplayCachedGlue);
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
    state.new_usertype<LuaDrawList>("SmatchetDrawList",
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
}

void AppController::LuaLogInfoBind(const std::string& msg) {
    const std::string clean = SanitizeLogText(msg);
    if (luaHost_ && !luaHost_->SnapshotLogSinks().empty()) {
        for (const auto& sink : luaHost_->SnapshotLogSinks()) {
            sink(clean);
        }
    } else {
        std::printf("[LUA] %s\n", clean.c_str());
    }
}

std::vector<CachedTicket> AppController::LuaGetActiveTicketsBind() {
    const auto snap = GetActiveTicketsSnapshot();
    return std::vector<CachedTicket>(snap->begin(), snap->end());
}

std::tuple<sol::object, std::string> AppController::LuaGetTicketBind(const std::string& issueId) {
    CachedTicket ticket;
    if (Cache->TryGetTicket(issueId, ticket)) {
        return {sol::make_object(lua, ticket), ""};
    }
    return {sol::make_object(lua, sol::nil), "Ticket not found in local cache"};
}

std::tuple<sol::object, std::string> AppController::LuaDecodeJsonBind(const std::string& s) {
    constexpr size_t kMaxDecodeBytes = 4u * 1024u * 1024u;
    if (s.size() > kMaxDecodeBytes) {
        return {sol::make_object(lua, sol::nil), std::string("input too large")};
    }
    try {
        const nlohmann::json j =
            nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/false);
        return {JsonToLua(lua, j), std::string()};
    } catch (const std::exception& e) {
        return {sol::make_object(lua, sol::nil), std::string(e.what())};
    }
}

std::tuple<sol::object, std::string> AppController::LuaCreateIssueBind(sol::table spec) {
    const TrackerConfig cfg = ConfigManager::Load();
    // Same base as the grid new-issue row: config fallbacks plus last-row project / issue type when present.
    IssueDraft draft = BuildDraftFromLastTicket(cfg);

    sol::object offlineObj = spec["offline"];
    if (!offlineObj.valid() || offlineObj.get_type() == sol::type::lua_nil) {
        offlineObj = spec["queue_offline"];
    }
    const bool offline = LuaTruthy(offlineObj);

    LuaMergeIssueCreateSpec(draft, std::move(spec), AvailableFields);

    sol::table result = lua.create_table();

    if (offline) {
        if (!Cache) {
            return {sol::make_object(lua, sol::nil),
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
        return {sol::make_object(lua, sol::nil),
                std::string("create_issue failed while waiting for result: ") + e.what()};
    } catch (...) {
        return {sol::make_object(lua, sol::nil),
                std::string("create_issue failed while waiting for result: unknown exception")};
    }

    result["ok"] = r.Ok;
    if (!r.IssueKey.empty()) {
        result["issue_key"] = r.IssueKey;
    }
    result["error"] = r.Error;
    if (!r.MissingFieldIds.empty()) {
        sol::table miss = lua.create_table();
        std::size_t i = 1;
        for (const auto& id : r.MissingFieldIds) {
            miss[i++] = id;
        }
        result["missing_field_ids"] = miss;
    }
    if (!r.AttachmentFailures.empty()) {
        sol::table af = lua.create_table();
        std::size_t i = 1;
        for (const auto& p : r.AttachmentFailures) {
            sol::table row = lua.create_table();
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

void AppController::LuaImGuiTextBind(const std::string& s) {
    ImGui::TextUnformatted(s.c_str());
}

void AppController::LuaImGuiTextUnformattedBind(const std::string& s) {
    ImGui::TextUnformatted(s.c_str());
}

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
            [self, opCap = std::move(op)]() mutable {
                self->ApplyOrQueueLuaWindowOp(std::move(opCap));
            });
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
            [self, opCap = std::move(op)]() mutable {
                self->ApplyOrQueueLuaWindowOp(std::move(opCap));
            });
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
            [self, opCap = std::move(op)]() mutable {
                self->ApplyOrQueueLuaWindowOp(std::move(opCap));
            });
    }
}

void AppController::LuaUiInvalidateFieldCacheBind(sol::optional<std::string> ticketId,
                                                  sol::optional<std::string> fieldId) {
    const std::string tid = ticketId.value_or(std::string());
    const std::string fid = fieldId.value_or(std::string());
    const bool hasTicket = static_cast<bool>(ticketId);
    const bool hasField  = static_cast<bool>(fieldId);
    auto apply = [this, hasTicket, hasField, tid, fid]() {
        if (!hasTicket) { luaFieldCache_.clear(); return; }
        if (!hasField) {
            for (auto it = luaFieldCache_.begin(); it != luaFieldCache_.end(); ) {
                const std::string& key = it->first;
                const std::size_t nul = key.find('\0');
                if (nul != std::string::npos && key.compare(0, nul, tid) == 0)
                    it = luaFieldCache_.erase(it);
                else ++it;
            }
            return;
        }
        std::string key = tid; key.push_back('\0'); key.append(fid);
        luaFieldCache_.erase(key);
    };
    if (IsOnUiThread()) {
        apply();
    } else {
        mainThreadDispatcher.PostToMainThread(std::move(apply));
    }
}

void AppController::NotifyLuaTicketDataChanged() {
    luaWindowDataGen_.fetch_add(1);
}

void AppController::LuaUiRegisterTicketActionBind(const std::string& name, const std::string& callbackFuncName) {
    luaTicketActions_.erase(std::remove_if(luaTicketActions_.begin(), luaTicketActions_.end(),
                                         [&](const std::pair<std::string, std::string>& p) {
                                             return p.first == name;
                                         }),
                          luaTicketActions_.end());
    if (!callbackFuncName.empty()) {
        luaTicketActions_.push_back({name, callbackFuncName});
    }
}

void AppController::LuaUiRegisterGlobalActionBind(const std::string& name, const std::string& callbackFuncName) {
    luaGlobalActions_.erase(std::remove_if(luaGlobalActions_.begin(), luaGlobalActions_.end(),
                                         [&](const std::pair<std::string, std::string>& p) {
                                             return p.first == name;
                                         }),
                          luaGlobalActions_.end());
    if (!callbackFuncName.empty()) {
        luaGlobalActions_.push_back({name, callbackFuncName});
    }

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
            c.Idempotent = false;   // Lua actions may mutate state
            c.AsyncSafe = true;
            // Capture by value so the handler owns a copy of the callback name string.
            const std::string cbName = callbackFuncName;
            AppController* appPtr = this;
            c.Handler = [appPtr, cbName](const nlohmann::json& /*args*/,
                                         const smatchet::cmd::CommandContext& /*ctx*/) {
                appPtr->ExecuteLuaGlobalAction(cbName);
                return smatchet::cmd::CommandResult::Success(nlohmann::json::object());
            };
            try {
                commandRegistry_->Register(std::move(c));
            } catch (const std::exception& ex) {
                LOG_WARN("LuaUiRegisterGlobalActionBind: could not register '%s' in registry: %s",
                         cmdName.c_str(), ex.what());
            }
        }
    }
}

void AppController::LuaMcpRegisterToolBind(sol::table toolDef, sol::function callback) {
    if (!toolDef.valid() || !callback.valid()) {
        return;
    }
    McpToolDefinition def;
    def.name = toolDef.get_or<std::string>("name", "");
    def.description = toolDef.get_or<std::string>("description", "");

    sol::object params = toolDef["parameters"];
    if (params.is<sol::table>()) {
        def.parametersSchema = LuaToJson(params);
    } else {
        std::string schemaStr = toolDef.get_or<std::string>("parameters_json", "{}");
        try {
            def.parametersSchema = nlohmann::json::parse(schemaStr);
        } catch (...) {
            def.parametersSchema = nlohmann::json::object();
        }
    }

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

        // Snapshot activeSetupScripts_ under the same mutex used by RunLuaSetupScript so the
        // iteration below sees a stable view even if the UI thread mutates the vector mid-job.
        std::vector<std::string> setupScriptsSnapshot;
        {
            std::lock_guard<std::mutex> lock(automationJobMutex_);
            setupScriptsSnapshot = activeSetupScripts_;
        }

        try {
            sol::state bgState;
            InitLuaCore(bgState);

            sol::environment sandbox = CreateSandboxEnvironment(bgState);

            // Load and run setup scripts so global actions are defined for this job.
            //
            // Lifecycle contract (backlog #34): the worker uses a *fresh* sol::state per job
            // for isolation, so every top-level statement in a setup script re-fires on every
            // job. Setup scripts MUST therefore be defining-only — declaring functions, tables,
            // constants — and avoid side effects at module-load (no `tracker.create_issue(...)`
            // at the top level, no `os.execute(...)` outside of a function body, etc.). Wrap any
            // such side-effecting work in a function that the job explicitly invokes, and the
            // re-execution becomes harmless. Caching compiled sol::function refs across jobs
            // would not change this: bytecode bound to a destroyed state cannot be replayed,
            // and persisting the state across jobs would lose the isolation guarantee.
            for (const auto& path : setupScriptsSnapshot) {
                std::string resolved = ResolveLuaScriptPath(path);
                if (resolved.empty()) {
                    continue;
                }
                auto script = bgState.load_file(resolved);
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

    lua_sethook(state.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) {
        sol::state_view sv(L);
        AppController* app = sv["__smatchet_app"].get_or<AppController*>(nullptr);
        if (app && app->shuttingDown_.load()) {
            luaL_error(L, "Script execution aborted (shutdown).");
        }
    }, LUA_MASKCOUNT, 50000);

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
            std::printf("%s\n", decorated.c_str());
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

    lua_sethook(lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) {
        luaL_error(L, "Script execution timeout exceeded.");
    }, LUA_MASKCOUNT, 100000);

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

// LuaDrawList method definitions — class declared in the anonymous namespace at the top of
// this TU. See docs/design/applied/lua-recorded-cmd-list.md § Crash-safety hardening for per-method
// input bounds; out-of-line so the class is complete-typed before InitLuaUi registers it.

namespace {

void LuaDrawList::RequireActive(const char* method) {
    if (!active_) {
        // sol2 catches std::exception thrown from C++ inside a Lua pcall frame and surfaces
        // it as a Lua error. The Lua-as-C++ build mode (CMakeLists.txt LANGUAGE CXX) makes
        // this unwinding safe through the recorder's std::vector / std::string temporaries.
        std::string msg = "draw:";
        msg += method;
        msg += " called outside its recording window";
        throw std::runtime_error(msg);
    }
}

AppController::ImCmd* LuaDrawList::LastInteractive() {
    for (auto it = cmds_.rbegin(); it != cmds_.rend(); ++it) {
        if (it->op == AppController::ImCmd::Op::Button ||
            it->op == AppController::ImCmd::Op::InputText) {
            return &(*it);
        }
    }
    return nullptr;
}

void LuaDrawList::Text(std::string s) {
    RequireActive("text");
    TruncateInPlace(s, kRecorderStringCap);
    AppController::ImCmd c;
    c.op = AppController::ImCmd::Op::Text;
    c.str = std::move(s);
    cmds_.push_back(std::move(c));
}

void LuaDrawList::TextUnformatted(std::string s) {
    RequireActive("text_unformatted");
    TruncateInPlace(s, kRecorderStringCap);
    AppController::ImCmd c;
    c.op = AppController::ImCmd::Op::TextUnformatted;
    c.str = std::move(s);
    cmds_.push_back(std::move(c));
}

void LuaDrawList::Image(std::string path, float w, float h) {
    RequireActive("image");
    if (path.empty()) return;
    if (!IsFiniteF(w) || !IsFiniteF(h) || w < 0.0f || h < 0.0f ||
        w > kRecorderSizeCap || h > kRecorderSizeCap) return;
    AppController::ImCmd c;
    c.op = AppController::ImCmd::Op::Image;
    c.str = std::move(path);
    c.f1 = w;
    c.f2 = h;
    cmds_.push_back(std::move(c));
}

void LuaDrawList::ProgressBar(float frac, float w, float h, sol::optional<std::string> overlay) {
    RequireActive("progress_bar");
    if (!IsFiniteF(frac)) frac = 0.0f;
    frac = ClampFloat(frac, 0.0f, 1.0f);
    if (!IsFiniteF(w) || !IsFiniteF(h)) return;
    // Negative w / h means "auto"; ImGui handles them at replay.
    if (w < -1.0f || w > kRecorderSizeCap) return;
    if (h < -1.0f || h > kRecorderSizeCap) return;
    AppController::ImCmd c;
    c.op = AppController::ImCmd::Op::ProgressBar;
    c.f1 = frac;
    c.f2 = w;
    c.f3 = h;
    if (overlay) {
        std::string ov = overlay.value();
        TruncateInPlace(ov, kRecorderStringCap);
        c.str = std::move(ov);
    }
    cmds_.push_back(std::move(c));
}

void LuaDrawList::SameLine(sol::optional<float> offset, sol::optional<float> spacing) {
    RequireActive("same_line");
    AppController::ImCmd c;
    c.op = AppController::ImCmd::Op::SameLine;
    const float off = offset ? offset.value() : 0.0f;
    const float sp  = spacing ? spacing.value() : -1.0f;
    c.f1 = IsFiniteF(off) ? ClampFloat(off, -kRecorderOffsetCap, kRecorderOffsetCap) : 0.0f;
    c.f2 = IsFiniteF(sp)  ? ClampFloat(sp,  -kRecorderOffsetCap, kRecorderOffsetCap) : -1.0f;
    cmds_.push_back(std::move(c));
}

void LuaDrawList::Separator() {
    RequireActive("separator");
    AppController::ImCmd c;
    c.op = AppController::ImCmd::Op::Separator;
    cmds_.push_back(std::move(c));
}

void LuaDrawList::Dummy(float w, float h) {
    RequireActive("dummy");
    if (!IsFiniteF(w) || !IsFiniteF(h) || w < 0.0f || h < 0.0f ||
        w > kRecorderSizeCap || h > kRecorderSizeCap) return;
    AppController::ImCmd c;
    c.op = AppController::ImCmd::Op::Dummy;
    c.f1 = w;
    c.f2 = h;
    cmds_.push_back(std::move(c));
}

void LuaDrawList::PushColor(int idx, float r, float g, float b, float a) {
    RequireActive("push_color");
    if (idx < 0 || idx >= ImGuiCol_COUNT) return;
    AppController::ImCmd c;
    c.op = AppController::ImCmd::Op::PushColor;
    c.i1 = idx;
    c.f1 = ClampFloat(IsFiniteF(r) ? r : 0.0f, 0.0f, 1.0f);
    c.f2 = ClampFloat(IsFiniteF(g) ? g : 0.0f, 0.0f, 1.0f);
    c.f3 = ClampFloat(IsFiniteF(b) ? b : 0.0f, 0.0f, 1.0f);
    c.f4 = ClampFloat(IsFiniteF(a) ? a : 1.0f, 0.0f, 1.0f);
    cmds_.push_back(std::move(c));
}

void LuaDrawList::PopColor(sol::optional<int> count) {
    RequireActive("pop_color");
    AppController::ImCmd c;
    c.op = AppController::ImCmd::Op::PopColor;
    c.i1 = count.value_or(1);
    if (c.i1 <= 0) c.i1 = 1;
    cmds_.push_back(std::move(c));
}

void LuaDrawList::SetTooltip(std::string s) {
    RequireActive("set_tooltip");
    TruncateInPlace(s, kRecorderStringCap);
    AppController::ImCmd c;
    c.op = AppController::ImCmd::Op::SetTooltip;
    c.str = std::move(s);
    cmds_.push_back(std::move(c));
}

void LuaDrawList::Button(std::string label, sol::protected_function onClick) {
    RequireActive("button");
    TruncateInPlace(label, kRecorderLabelCap);
    // Auto-disambiguate IDs across multiple interactive widgets in the same recording.
    // The cell / window outer ID scope handles cross-cell / cross-window collisions.
    AppController::ImCmd c;
    c.op = AppController::ImCmd::Op::Button;
    c.str = std::move(label);
    c.str.append("##b");
    c.str.append(std::to_string(interactiveIndex_++));
    if (onClick.valid()) {
        c.callback = std::move(onClick);
    }
    cmds_.push_back(std::move(c));
}

void LuaDrawList::InputText(std::string label, std::string initial,
                            int maxLen, sol::protected_function onCommit) {
    RequireActive("input_text");
    TruncateInPlace(label, kRecorderLabelCap);
    if (maxLen <= 0) maxLen = 256;
    if (maxLen > kRecorderMaxLen) maxLen = kRecorderMaxLen;
    if (static_cast<int>(initial.size()) > maxLen) initial.resize(maxLen);
    AppController::ImCmd c;
    c.op = AppController::ImCmd::Op::InputText;
    c.str = std::move(label);
    c.str.append("##it");
    c.str.append(std::to_string(interactiveIndex_++));
    c.i1 = maxLen;
    c.textBuf.assign(maxLen + 1, '\0');
    if (!initial.empty()) {
        const std::size_t n = std::min<std::size_t>(initial.size(), c.textBuf.size() - 1);
        std::memcpy(c.textBuf.data(), initial.data(), n);
    }
    if (onCommit.valid()) {
        c.callback = std::move(onCommit);
    }
    cmds_.push_back(std::move(c));
}

void LuaDrawList::OnDeactivated(sol::protected_function fn) {
    RequireActive("on_deactivated");
    AppController::ImCmd* last = LastInteractive();
    if (!last) {
        LOG_WARN("LuaDrawList::on_deactivated called with no prior interactive op; ignoring");
        return;
    }
    if (fn.valid()) last->onDeactivated = std::move(fn);
}

void LuaDrawList::OnDeactivatedAfterEdit(sol::protected_function fn) {
    RequireActive("on_deactivated_after_edit");
    AppController::ImCmd* last = LastInteractive();
    if (!last) {
        LOG_WARN("LuaDrawList::on_deactivated_after_edit called with no prior interactive op; ignoring");
        return;
    }
    if (fn.valid()) last->onDeactivatedAfterEdit = std::move(fn);
}

enum class LuaReplayCallbackKind : std::uint8_t {
    None        = 0,
    Click       = 1u << 0,
    Commit      = 1u << 1,
    Deactivated = 1u << 2,
};

void InvokeLuaCallbackSandboxed(sol::state& lua, sol::protected_function& fn,
                                const std::string& cbArg1, const std::string& cbArg2) {
    if (!fn.valid()) return;
    try {
        LuaHookGuard hook(lua);
        sol::protected_function_result r = fn(cbArg1, cbArg2);
        if (!r.valid()) {
            sol::error e = r;
            LOG_WARN("Lua callback error: %s", e.what());
        }
    } catch (const std::exception& ex) {
        LOG_WARN("Lua callback C++ exception: %s", ex.what());
    } catch (...) {
        LOG_WARN("Lua callback unknown C++ exception");
    }
}

void InvokeLuaCallbackSandboxed3(sol::state& lua, sol::protected_function& fn,
                                 const std::string& cbArg1, const std::string& cbArg2,
                                 const std::string& cbArg3) {
    if (!fn.valid()) return;
    try {
        LuaHookGuard hook(lua);
        sol::protected_function_result r = fn(cbArg1, cbArg2, cbArg3);
        if (!r.valid()) {
            sol::error e = r;
            LOG_WARN("Lua callback error: %s", e.what());
        }
    } catch (const std::exception& ex) {
        LOG_WARN("Lua callback C++ exception: %s", ex.what());
    } catch (...) {
        LOG_WARN("Lua callback unknown C++ exception");
    }
}

std::uint8_t ReplayCmdList(std::vector<AppController::ImCmd>& cmds, AppController& app,
                           sol::state& lua, const std::string& cbArg1, const std::string& cbArg2,
                           bool isReadOnly) {
    SMATCHET_UI_PERF_SCOPE("LuaDrawList::Replay");
    int          pushed = 0;
    std::uint8_t fired  = 0;
    using K = LuaReplayCallbackKind;
    using Op = AppController::ImCmd::Op;
    try {
        for (AppController::ImCmd& c : cmds) {
            switch (c.op) {
                case Op::Text:            ImGui::Text("%s", c.str.c_str()); break;
                case Op::TextUnformatted: ImGui::TextUnformatted(c.str.c_str()); break;
                case Op::Image:
                    SmatchetFieldIconRender::DrawImagePathOrUrl(app, c.str, c.f1, c.f2);
                    break;
                case Op::ProgressBar: {
                    ImVec2 sz(c.f2, c.f3);
                    if (c.f2 < 0.0f)  sz.x = ImGui::GetContentRegionAvail().x;
                    if (c.f3 <= 0.0f) sz.y = ImGui::GetFrameHeight();
                    ImGui::ProgressBar(c.f1, sz, c.str.empty() ? nullptr : c.str.c_str());
                    break;
                }
                case Op::SameLine:  ImGui::SameLine(c.f1, c.f2); break;
                case Op::Separator: ImGui::Separator(); break;
                case Op::Dummy:     ImGui::Dummy(ImVec2(c.f1, c.f2)); break;
                case Op::PushColor:
                    if (c.i1 < 0 || c.i1 >= ImGuiCol_COUNT) break;
                    ImGui::PushStyleColor(c.i1, ImVec4(c.f1, c.f2, c.f3, c.f4));
                    ++pushed;
                    break;
                case Op::PopColor: {
                    int n = c.i1 > 0 ? c.i1 : 1;
                    if (n > pushed) n = pushed;
                    if (n > 0) { ImGui::PopStyleColor(n); pushed -= n; }
                    break;
                }
                case Op::SetTooltip:
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.str.c_str());
                    break;
                case Op::Button: {
                    // Read-only cells get visually disabled + no callback firing. Wrap the
                    // call in BeginDisabled so click events are suppressed at the ImGui layer
                    // (no input routing, no hover-active styling) and we don't have to
                    // double-check IsItemDeactivated* — the disabled item still reports
                    // deactivation when focus moves on, so explicit isReadOnly guards remain.
                    if (isReadOnly) ImGui::BeginDisabled();
                    const bool clicked = ImGui::Button(c.str.c_str());
                    if (isReadOnly) ImGui::EndDisabled();
                    if (!isReadOnly) {
                        if (clicked && c.callback) {
                            InvokeLuaCallbackSandboxed(lua, c.callback, cbArg1, cbArg2);
                            fired |= static_cast<std::uint8_t>(K::Click);
                        }
                        if (c.onDeactivated && ImGui::IsItemDeactivated()) {
                            InvokeLuaCallbackSandboxed(lua, c.onDeactivated, cbArg1, cbArg2);
                            fired |= static_cast<std::uint8_t>(K::Deactivated);
                        }
                        if (c.onDeactivatedAfterEdit && ImGui::IsItemDeactivatedAfterEdit()) {
                            InvokeLuaCallbackSandboxed(lua, c.onDeactivatedAfterEdit, cbArg1, cbArg2);
                            fired |= static_cast<std::uint8_t>(K::Deactivated);
                        }
                    }
                    break;
                }
                case Op::InputText: {
                    if (c.textBuf.empty()) break;
                    // Encode read_only into the ImGui call directly (input is rejected at the
                    // text widget layer, no need to skip-render). Callbacks are still gated:
                    // a ReadOnly InputText never fires IsItemDeactivatedAfterEdit-with-commit
                    // in practice, but the explicit guard keeps the invariant tight even if
                    // a future ImGui revision changes that behaviour.
                    const ImGuiInputTextFlags flags =
                        isReadOnly ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None;
                    ImGui::InputText(c.str.c_str(), c.textBuf.data(), c.textBuf.size(), flags);
                    if (!isReadOnly) {
                        if (ImGui::IsItemDeactivatedAfterEdit() && c.callback) {
                            InvokeLuaCallbackSandboxed3(lua, c.callback, cbArg1, cbArg2,
                                                        std::string(c.textBuf.data()));
                            fired |= static_cast<std::uint8_t>(K::Commit);
                        }
                        if (c.onDeactivated && ImGui::IsItemDeactivated()) {
                            InvokeLuaCallbackSandboxed(lua, c.onDeactivated, cbArg1, cbArg2);
                            fired |= static_cast<std::uint8_t>(K::Deactivated);
                        }
                        if (c.onDeactivatedAfterEdit && ImGui::IsItemDeactivatedAfterEdit()) {
                            InvokeLuaCallbackSandboxed(lua, c.onDeactivatedAfterEdit, cbArg1, cbArg2);
                            fired |= static_cast<std::uint8_t>(K::Deactivated);
                        }
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
    if (pushed > 0) ImGui::PopStyleColor(pushed);
    return fired;
}

} // namespace

bool AppController::TryRenderCachedLuaField(const std::string& fieldId, const CachedTicket& ticket,
                                            const std::string& rawValue, float availWidth,
                                            const TrackerField* fieldMeta, bool allowEdits) {
    // 1. Provider lookup — by field id first, then lowercased display name.
    sol::protected_function* providerSlot = nullptr;
    {
        const auto itId = fieldDisplayCachedProviders_.find(fieldId);
        if (itId != fieldDisplayCachedProviders_.end() && itId->second.valid()) {
            providerSlot = &itId->second;
        } else if (fieldMeta && !fieldMeta->Name.empty() &&
                   !fieldDisplayCachedProvidersByName_.empty()) {
            const auto itName = fieldDisplayCachedProvidersByName_.find(AsciiLowerCopy(fieldMeta->Name));
            if (itName != fieldDisplayCachedProvidersByName_.end() && itName->second.valid()) {
                providerSlot = &itName->second;
            }
        }
    }
    if (providerSlot == nullptr) return false;

    const bool catalogReadOnly  = fieldMeta ? fieldMeta->ReadOnly : false;
    const bool editMetaReadOnly = !CanEditFieldForIssue(ticket.id, fieldId, fieldMeta);
    const bool isReadOnly       = catalogReadOnly || editMetaReadOnly || !allowEdits;
    const std::string fieldName = fieldMeta ? fieldMeta->Name : std::string();
    const int intAvailWidth     = static_cast<int>(std::lround(availWidth));
    const std::uint64_t curProviderGen = luaProviderGen_.load();

    // 2. Cache key + hit check.
    std::string key = ticket.id;
    key.push_back('\0');
    key.append(fieldId);
    auto cit = luaFieldCache_.find(key);
    if (cit != luaFieldCache_.end()) {
        LuaFieldCacheEntry& entry = cit->second;
        const bool inputsMatch =
            entry.rawValue == rawValue &&
            entry.fieldName == fieldName &&
            entry.intAvailWidth == intAvailWidth &&
            entry.isReadOnly == isReadOnly &&
            entry.providerGen == curProviderGen;
        if (inputsMatch) {
            if (!entry.handled) return false;
            ReplayCmdList(entry.cmds, *this, lua, ticket.id, fieldId, entry.isReadOnly);
            return true;
        }
    }

    // 3. Miss / stale — invoke provider on a fresh recorder.
    SMATCHET_UI_PERF_SCOPE("LuaDrawList::Record");
    sol::protected_function providerCopy = *providerSlot;   // crash-safety §C3 (own a ref)
    auto rec = std::make_shared<LuaDrawList>();             // F6 lifetime: shared with Lua
    sol::object fieldNameObj = fieldMeta ? sol::make_object(lua, fieldMeta->Name)
                                         : sol::make_object(lua, sol::nil);
    bool callOk = true;
    std::string cxxExceptionMsg;
    sol::protected_function_result res;
    try {
        LuaHookGuard hook(lua);
        LuaImmediateModeGuard imm(false);
        res = providerCopy(ticket.id, fieldId, rawValue, availWidth, isReadOnly, fieldNameObj, rec);
    } catch (const std::exception& ex) {
        callOk = false;
        cxxExceptionMsg = ex.what();
        LOG_WARN("TryRenderCachedLuaField: exception field=%s err=%s", fieldId.c_str(), ex.what());
    } catch (...) {
        callOk = false;
        cxxExceptionMsg = "unknown C++ exception";
        LOG_WARN("TryRenderCachedLuaField: unknown exception field=%s", fieldId.c_str());
    }
    rec->Deactivate();

    LuaFieldCacheEntry entry;
    entry.rawValue      = rawValue;
    entry.fieldName     = fieldName;
    entry.intAvailWidth = intAvailWidth;
    entry.isReadOnly    = isReadOnly;
    entry.providerGen   = curProviderGen;
    if (!callOk || !res.valid()) {
        // Surface the error in the persistent "Lua Errors" panel + scrolling log + auto-
        // open Scripting window so the user can debug instead of staring at a silent C++
        // fallback. Cache-key is populated below, so this error fires once per cell per
        // (rawValue, fieldName, width, readOnly, providerGen) tuple — never per frame.
        std::string errMsg;
        if (!callOk) {
            errMsg = cxxExceptionMsg;
        } else {
            sol::error e = res;
            errMsg = e.what();
            LOG_WARN("TryRenderCachedLuaField: Lua error field=%s err=%s",
                     fieldId.c_str(), e.what());
        }
        const std::string bare = "[LUA cell] field=" + fieldName + " id=" + ticket.id + ": " + errMsg;
        LuaLogInfoBind(std::string("[ERROR] ") + bare);
        for (const auto& sink : errorSinks_) {
            sink(bare);
        }
        scriptingWindowOpenRequested_.store(true);
        entry.handled = false;
        entry.cmds.clear();
    } else {
        bool truthy = false;
        if (res.return_count() >= 1) {
            sol::object ret = res.get<sol::object>(0);
            truthy = LuaTruthy(ret);
        }
        if (truthy) {
            entry.handled = true;
            entry.cmds = rec->Take();
        } else {
            entry.handled = false;
        }
    }

    const bool handled = entry.handled;
    if (handled) {
        // Replay the freshly-recorded list immediately so this frame paints.
        ReplayCmdList(entry.cmds, *this, lua, ticket.id, fieldId, entry.isReadOnly);
    }
    luaFieldCache_[key] = std::move(entry);
    return handled;
}

bool AppController::ScenarioRegisterLuaCachedProvider(const std::string& fieldId,
                                                      const std::string& luaFnName,
                                                      std::string& outError) {
    return ScenarioRegisterLuaCachedProvider(fieldId, luaFnName, std::vector<std::string>{},
                                             outError);
}

bool AppController::ScenarioRegisterLuaCachedProvider(const std::string& fieldId,
                                                      const std::string& luaFnName,
                                                      const std::vector<std::string>& extraScripts,
                                                      std::string& outError) {
    namespace fs = ghc::filesystem;
    outError.clear();
    if (fieldId.empty() || luaFnName.empty()) {
        outError = "fieldId and luaFnName required";
        return false;
    }

    auto lookup = [&]() -> sol::protected_function {
        sol::object slot = lua[luaFnName];
        if (slot.valid() && slot.get_type() == sol::type::function) {
            return slot.as<sol::protected_function>();
        }
        return sol::protected_function();
    };

    sol::protected_function fn = lookup();

    if (!fn.valid()) {
        // Function may live in a script the ephemeral / headless launcher hasn't loaded yet
        // (userData/Scripts is empty on a fresh install). Probe a candidate-path list and
        // load the first hit in the global env via `lua.script_file` so subsequent `_G[name]`
        // lookup succeeds. Non-`local` `function` declarations become globally visible.
        std::vector<std::string> filenames;
        filenames.emplace_back("SmatchetHooks.lua");
        for (const std::string& extra : extraScripts) {
            filenames.push_back(extra);
        }

        std::vector<std::string> roots;
        if (!luaScriptsDirectory_.empty()) {
            roots.push_back(luaScriptsDirectory_);
        }
        roots.emplace_back("Scripts/");
        roots.emplace_back("scripts/");
        roots.emplace_back("../scripts/");
        roots.emplace_back("../../scripts/");
        roots.emplace_back("../../../scripts/");

        std::string loadErr;
        for (const std::string& fname : filenames) {
            for (const std::string& root : roots) {
                const std::string path = root + fname;
                std::error_code ec;
                if (!fs::is_regular_file(fs::path(path), ec)) {
                    continue;
                }
                try {
                    sol::protected_function_result r = lua.script_file(path);
                    if (!r.valid()) {
                        sol::error e = r;
                        loadErr = path + ": " + e.what();
                        continue;
                    }
                    LOG_INFO("ScenarioRegisterLuaCachedProvider: auto-loaded %s", path.c_str());
                    fn = lookup();
                    if (fn.valid()) {
                        break;
                    }
                } catch (const std::exception& ex) {
                    loadErr = path + ": " + ex.what();
                }
            }
            if (fn.valid()) {
                break;
            }
        }

        if (!fn.valid()) {
            outError = "Lua function not found or not callable: " + luaFnName;
            if (!loadErr.empty()) {
                outError += " (auto-load tried: " + loadErr + ")";
            }
            return false;
        }
    }

    // Stash any pre-existing user-side provider so Unregister can restore it. Tracked
    // separately for "had a provider" vs "had nothing" so we never accidentally synthesise
    // an empty entry on restore.
    auto existing = fieldDisplayCachedProviders_.find(fieldId);
    if (existing != fieldDisplayCachedProviders_.end()) {
        scenarioPriorFieldProviders_[fieldId] = existing->second;
        scenarioPriorEmptyFields_.erase(fieldId);
    } else {
        scenarioPriorFieldProviders_.erase(fieldId);
        scenarioPriorEmptyFields_.insert(fieldId);
    }

    fieldDisplayCachedProviders_[fieldId] = std::move(fn);
    luaProviderGen_.fetch_add(1);
    LOG_INFO("ScenarioRegisterLuaCachedProvider: bound field=%s fn=%s", fieldId.c_str(),
             luaFnName.c_str());
    return true;
}

void AppController::ScenarioUnregisterLuaCachedProvider(const std::string& fieldId) {
    // Restore the user-side provider that Register displaced, or erase if there was none.
    auto priorIt    = scenarioPriorFieldProviders_.find(fieldId);
    const bool hadPrior     = (priorIt != scenarioPriorFieldProviders_.end());
    const bool hadEmptyPrior = scenarioPriorEmptyFields_.count(fieldId) > 0;
    if (!hadPrior && !hadEmptyPrior) {
        // Field was never registered through the scenario surface — leave alone.
        return;
    }

    if (hadPrior) {
        fieldDisplayCachedProviders_[fieldId] = std::move(priorIt->second);
        scenarioPriorFieldProviders_.erase(priorIt);
    } else {
        fieldDisplayCachedProviders_.erase(fieldId);
    }
    scenarioPriorEmptyFields_.erase(fieldId);
    luaProviderGen_.fetch_add(1);

    for (auto it = luaFieldCache_.begin(); it != luaFieldCache_.end();) {
        const std::string& key = it->first;
        const std::size_t nul = key.find('\0');
        if (nul != std::string::npos && key.compare(nul + 1, std::string::npos, fieldId) == 0) {
            it = luaFieldCache_.erase(it);
        } else {
            ++it;
        }
    }
    LOG_INFO("ScenarioUnregisterLuaCachedProvider: field=%s restored=%s", fieldId.c_str(),
             hadPrior ? "user-provider" : "(none)");
}

void AppController::ScenarioInvalidateLuaFieldCache() {
    luaFieldCache_.clear();
}

bool AppController::TryGetFieldIconMapTarget(const std::string& fieldId, const TrackerField* field,
                                             const std::string& rawValue, std::string& outPathOrUrl) const {
    outPathOrUrl.clear();
    using TrackerFieldValueUtils::ResolveOptionId;
    using TrackerFieldValueUtils::ResolveOptionLabel;

    auto lookupInner = [&](const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& outer,
                           const std::string& outerKey) -> bool {
        const auto oit = outer.find(outerKey);
        if (oit == outer.end()) {
            return false;
        }
        const auto& inner = oit->second;
        std::vector<std::string> keys;
        if (field != nullptr) {
            const std::string optId = ToLowerAsciiCopy(TrimCopyAsciiWhitespace(ResolveOptionId(*field, rawValue)));
            if (!optId.empty()) {
                keys.push_back(optId);
                keys.push_back(ToLowerAsciiCopy(TrimCopyAsciiWhitespace(ResolveOptionLabel(*field, optId))));
            }
        }
        keys.push_back(ToLowerAsciiCopy(TrimCopyAsciiWhitespace(rawValue)));
        if (field != nullptr) {
            keys.push_back(
                ToLowerAsciiCopy(TrimCopyAsciiWhitespace(DisplayValueForTrackerDateField(fieldId, field, rawValue))));
        }
        for (const std::string& k : keys) {
            if (k.empty()) {
                continue;
            }
            const auto it = inner.find(k);
            if (it != inner.end() && !it->second.empty()) {
                outPathOrUrl = it->second;
                return true;
            }
        }
        return false;
    };

    std::lock_guard<std::mutex> lock(fieldIconMapsMutex_);
    if (lookupInner(fieldIconMapsByFieldId_, fieldId)) {
        return true;
    }
    if (field != nullptr && !field->Name.empty()) {
        if (lookupInner(fieldIconMapsByDisplayName_, ToLowerAsciiCopy(field->Name))) {
            return true;
        }
    }
    return false;
}


std::vector<AppController::McpToolDefinition> AppController::GetLuaMcpTools() const {
    std::lock_guard<std::mutex> lock(luaMcpToolsMutex_);
    return luaMcpTools_;
}

std::string AppController::ExecuteLuaMcpTool(const std::string& name, const std::string& paramsJson, std::string& outError) {
    sol::protected_function callback;
    {
        std::lock_guard<std::mutex> lock(luaMcpToolsMutex_);
        const auto it =
            std::find_if(luaMcpTools_.begin(), luaMcpTools_.end(),
                         [&](const McpToolDefinition& tool) { return tool.name == name; });
        if (it == luaMcpTools_.end()) {
            outError = "Tool not found";
            LOG_TRACE("ExecuteLuaMcpTool: not_found name=%s", name.c_str());
            return "";
        }
        callback = it->callback;
    }

    LOG_TRACE("ExecuteLuaMcpTool: begin name=%s params_len=%zu", name.c_str(), paramsJson.size());
    lua_sethook(lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) {
        luaL_error(L, "Script execution timeout exceeded.");
    }, LUA_MASKCOUNT, 100000);

    nlohmann::json jParams;
    try {
        jParams = nlohmann::json::parse(paramsJson);
    } catch (...) {
        jParams = nlohmann::json::object();
    }
    try {
        LOG_TRACE("ExecuteLuaMcpTool: params_json=%s", TruncateForTrace(jParams.dump()).c_str());
    } catch (...) {
        LOG_TRACE("ExecuteLuaMcpTool: params (dump failed)");
    }

    FieldEditAuditSource::ScopedOverride mcpSource(FieldEditAuditSource::kMcp);
    sol::protected_function_result result = callback(JsonToLua(lua, jParams));

    lua_sethook(lua.lua_state(), nullptr, 0, 0);

    if (!result.valid()) {
        sol::error e = result;
        outError = e.what();
        LOG_TRACE("ExecuteLuaMcpTool: error name=%s err=%s", name.c_str(), TruncateForTrace(outError).c_str());
        return "";
    }
    std::string ret;
    if (result.return_count() > 0) {
        sol::object obj = result[0];
        if (obj.is<std::string>()) {
            ret = obj.as<std::string>();
        } else {
            ret = LuaToJson(obj).dump();
        }
    } else {
        ret = "{}";
    }
    LOG_TRACE("ExecuteLuaMcpTool: ok name=%s result_len=%zu", name.c_str(), ret.size());
    return ret;
}


std::string AppController::ExecuteLuaSnippetForMcp(const std::string& code, const nlohmann::json& args,
                                                   std::string& outError) {
    if (code.empty()) {
        outError = "Missing snippet code";
        return "";
    }

    try {
        LOG_TRACE("ExecuteLuaSnippetForMcp: begin code_len=%zu args=%s", code.size(),
                  TruncateForTrace(args.dump()).c_str());
    } catch (...) {
        LOG_TRACE("ExecuteLuaSnippetForMcp: begin code_len=%zu (args dump failed)", code.size());
    }

    sol::environment sandbox = CreateSandboxEnvironment(lua);
    sandbox["args"] = JsonToLua(lua, args);

    sol::load_result script = lua.load(code, "mcp.run_lua.snippet");
    if (!script.valid()) {
        sol::error err = script;
        outError = err.what();
        lua_sethook(lua.lua_state(), nullptr, 0, 0);
        LOG_TRACE("ExecuteLuaSnippetForMcp: load_error %s", TruncateForTrace(outError).c_str());
        return "";
    }

    sol::protected_function func = script;
    sandbox.set_on(func);

    lua_sethook(lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) {
        luaL_error(L, "Script execution timeout exceeded.");
    }, LUA_MASKCOUNT, 100000);

    FieldEditAuditSource::ScopedOverride mcpSource(FieldEditAuditSource::kMcp);
    sol::protected_function_result result = func();
    lua_sethook(lua.lua_state(), nullptr, 0, 0);

    if (!result.valid()) {
        sol::error e = result;
        outError = e.what();
        LOG_TRACE("ExecuteLuaSnippetForMcp: runtime_error %s", TruncateForTrace(outError).c_str());
        return "";
    }
    std::string ret;
    if (result.return_count() > 0) {
        sol::object obj = result[0];
        if (obj.is<std::string>()) {
            ret = obj.as<std::string>();
        } else {
            ret = LuaToJson(obj).dump();
        }
    } else {
        ret = "{}";
    }
    LOG_TRACE("ExecuteLuaSnippetForMcp: ok result_len=%zu", ret.size());
    return ret;
}

bool AppController::ExecuteLuaConsoleSnippet(const std::string& code, std::string& outError,
                                             std::string& outResultSummary) {
    outError.clear();
    outResultSummary.clear();
    if (code.empty()) {
        outError = "No code to run";
        return false;
    }
    constexpr size_t kMaxConsoleSnippetBytes = 512u * 1024u;
    if (code.size() > kMaxConsoleSnippetBytes) {
        outError = "Code exceeds maximum size (512 KB)";
        return false;
    }

    LOG_TRACE("ExecuteLuaConsoleSnippet: begin code_len=%zu", code.size());

    sol::environment sandbox = CreateSandboxEnvironment(lua);
    sol::load_result script = lua.load(code, "lua_console.oneshot");
    if (!script.valid()) {
        sol::error err = script;
        outError = err.what();
        LOG_TRACE("ExecuteLuaConsoleSnippet: load_error %s", TruncateForTrace(outError).c_str());
        LuaLogInfoBind(std::string("[ERROR] ") + outError);
        for (const auto& sink : errorSinks_) {
            sink(outError);
        }
        scriptingWindowOpenRequested_.store(true);
        return false;
    }

    sol::protected_function func = script;
    sandbox.set_on(func);

    lua_sethook(lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) {
        luaL_error(L, "Script execution timeout exceeded.");
    }, LUA_MASKCOUNT, 100000);

    FieldEditAuditSource::ScopedOverride luaSource(FieldEditAuditSource::kLua);
    sol::protected_function_result result = func();
    lua_sethook(lua.lua_state(), nullptr, 0, 0);

    if (!result.valid()) {
        sol::error e = result;
        outError = e.what();
        LOG_TRACE("ExecuteLuaConsoleSnippet: runtime_error %s", TruncateForTrace(outError).c_str());
        // Red scrolling-log entry + persistent error panel + auto-open window.
        LuaLogInfoBind(std::string("[ERROR] ") + outError);
        for (const auto& sink : errorSinks_) {
            sink(outError);
        }
        scriptingWindowOpenRequested_.store(true);
        return false;
    }

    if (result.return_count() > 0) {
        try {
            const sol::object obj = result[0];
            if (obj.valid() && obj.get_type() != sol::type::lua_nil) {
                if (obj.is<std::string>()) {
                    outResultSummary = obj.as<std::string>();
                } else {
                    outResultSummary = LuaToJson(obj).dump();
                }
            }
        } catch (const std::exception& e) {
            outResultSummary = std::string("(return stringify failed: ") + e.what() + ")";
        } catch (...) {
            outResultSummary = "(return stringify failed)";
        }
        constexpr size_t kMaxSummary = 800;
        if (outResultSummary.size() > kMaxSummary) {
            outResultSummary.resize(kMaxSummary);
            outResultSummary += "...";
        }
    }

    LOG_TRACE("ExecuteLuaConsoleSnippet: ok summary_len=%zu", outResultSummary.size());
    return true;
}

std::string AppController::ExecuteLuaScriptForMcp(const std::string& scriptName, const nlohmann::json& args,
                                                  std::string& outError) {
    const std::string path = ResolveLuaScriptPath(scriptName);
    if (path.empty()) {
        outError = "Invalid script path";
        LOG_TRACE("ExecuteLuaScriptForMcp: invalid scriptName=%s", scriptName.c_str());
        return "";
    }

    try {
        LOG_TRACE("ExecuteLuaScriptForMcp: begin path=%s scriptName=%s args=%s", path.c_str(), scriptName.c_str(),
                  TruncateForTrace(args.dump()).c_str());
    } catch (...) {
        LOG_TRACE("ExecuteLuaScriptForMcp: begin path=%s (args dump failed)", path.c_str());
    }

    sol::environment sandbox = CreateSandboxEnvironment(lua);
    sandbox["args"] = JsonToLua(lua, args);

    sol::load_result script = lua.load_file(path);
    if (!script.valid()) {
        sol::error err = script;
        outError = err.what();
        lua_sethook(lua.lua_state(), nullptr, 0, 0);
        LOG_TRACE("ExecuteLuaScriptForMcp: load_error path=%s %s", path.c_str(), TruncateForTrace(outError).c_str());
        return "";
    }

    sol::protected_function func = script;
    sandbox.set_on(func);

    lua_sethook(lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) {
        luaL_error(L, "Script execution timeout exceeded.");
    }, LUA_MASKCOUNT, 100000);

    FieldEditAuditSource::ScopedOverride mcpSource(FieldEditAuditSource::kMcp);
    sol::protected_function_result result = func();
    lua_sethook(lua.lua_state(), nullptr, 0, 0);

    if (!result.valid()) {
        sol::error e = result;
        outError = e.what();
        LOG_TRACE("ExecuteLuaScriptForMcp: runtime_error path=%s %s", path.c_str(),
                  TruncateForTrace(outError).c_str());
        return "";
    }
    std::string ret;
    if (result.return_count() > 0) {
        sol::object obj = result[0];
        if (obj.is<std::string>()) {
            ret = obj.as<std::string>();
        } else {
            ret = LuaToJson(obj).dump();
        }
    } else {
        ret = "{}";
    }
    LOG_TRACE("ExecuteLuaScriptForMcp: ok path=%s result_len=%zu", path.c_str(), ret.size());
    return ret;
}

void AppController::DrawLuaWindows() {
    // Recorded-cmd-list draw path. The Lua draw fn runs only on a dirty / gen-mismatch
    // frame; otherwise we replay the cached recording. See plan §Window draw.
    inDrawLuaWindows_ = true;
    const std::uint64_t curDataGen     = luaWindowDataGen_.load();
    const std::uint64_t curProviderGen = luaProviderGen_.load();
    for (LuaWindowEntry& w : luaWindows_) {
        if (!w.drawFn.valid()) continue;
        bool open = true;
        if (ImGui::Begin(w.name.c_str(), &open)) {
            const bool needRecord = w.dirty ||
                                    w.cachedDataGen != curDataGen ||
                                    w.cachedProviderGen != curProviderGen;
            if (needRecord) {
                auto rec = std::make_shared<LuaDrawList>();
                FieldEditAuditSource::ScopedOverride luaSource(FieldEditAuditSource::kLua);
                sol::protected_function drawFnCopy = w.drawFn;   // Crash-safety §C3
                bool callOk = true;
                std::string cxxErr;
                sol::protected_function_result res;
                try {
                    SMATCHET_UI_PERF_SCOPE("LuaWindow::Record");
                    LuaHookGuard hook(lua);
                    LuaImmediateModeGuard imm(false);
                    res = drawFnCopy(rec);
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
                    std::string msg;
                    if (!callOk) {
                        msg = cxxErr;
                    } else {
                        sol::error e = res;
                        msg = e.what();
                    }
                    LOG_TRACE("DrawLuaWindows: error window=%s %s", w.name.c_str(), msg.c_str());
                    // Surface in the persistent "Lua Errors" panel + scrolling log +
                    // auto-open Scripting window. Negative-cache below means this fires
                    // once per record-event per window, not per frame.
                    const std::string bare = "[LUA window] " + w.name + ": " + msg;
                    LuaLogInfoBind(std::string("[ERROR] ") + bare);
                    for (const auto& sink : errorSinks_) {
                        sink(bare);
                    }
                    scriptingWindowOpenRequested_.store(true);
                    w.cmds.clear();
                    w.hasError = true;
                    w.errorMessage = std::move(msg);
                    w.cachedDataGen = curDataGen;
                    w.cachedProviderGen = curProviderGen;
                    w.dirty = false;
                }
            }
            if (w.hasError) {
                ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Lua Error: %s",
                                   w.errorMessage.c_str());
            } else {
                using K = LuaReplayCallbackKind;
                std::uint8_t fired;
                {
                    SMATCHET_UI_PERF_SCOPE("LuaWindow::Replay");
                    fired = ReplayCmdList(w.cmds, *this, lua, w.name, std::string(),
                                          /*isReadOnly=*/false);
                }
                const std::uint8_t dirtyMask =
                    static_cast<std::uint8_t>(K::Click) | static_cast<std::uint8_t>(K::Commit);
                // Per plan Q6: only Click / Commit auto-dirty; on_deactivated* does not.
                if (fired & dirtyMask) {
                    w.dirty = true;
                }
            }
        }
        ImGui::End();
        if (!open) w.drawFn = sol::lua_nil;
    }
    inDrawLuaWindows_ = false;

    // Drain ops enqueued mid-iteration (button callbacks that called register/unregister/
    // invalidate). ApplyOrQueueLuaWindowOp now hits the direct-mutation path because the
    // flag flipped to false.
    if (!pendingLuaWindowOps_.empty()) {
        std::vector<PendingLuaWindowOp> drained;
        drained.swap(pendingLuaWindowOps_);
        for (PendingLuaWindowOp& op : drained) {
            ApplyOrQueueLuaWindowOp(std::move(op));
        }
    }

    luaWindows_.erase(std::remove_if(luaWindows_.begin(), luaWindows_.end(),
        [](const LuaWindowEntry& w) { return !w.drawFn.valid(); }), luaWindows_.end());
}

std::vector<std::string> AppController::GetLuaTicketActionNames() const {
    std::vector<std::string> names;
    names.reserve(luaTicketActions_.size());
    std::transform(luaTicketActions_.begin(), luaTicketActions_.end(), std::back_inserter(names),
                   [](const auto& pair) { return pair.first; });
    return names;
}


std::vector<std::string> AppController::GetLuaGlobalActionNames() const {
    std::vector<std::string> names;
    names.reserve(luaGlobalActions_.size());
    std::transform(luaGlobalActions_.begin(), luaGlobalActions_.end(), std::back_inserter(names),
                   [](const auto& pair) { return pair.first; });
    return names;
}


void AppController::ExecuteLuaTicketAction(const std::string& name, const std::string& issueId) {
    std::string callbackFuncName;
    const auto it =
        std::find_if(luaTicketActions_.begin(), luaTicketActions_.end(),
                     [&](const std::pair<std::string, std::string>& pair) { return pair.first == name; });
    if (it != luaTicketActions_.end()) {
        callbackFuncName = it->second;
    }
    if (callbackFuncName.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(automationJobMutex_);
        automationJobs_.push_back({AutomationJob::Type::TicketAction, callbackFuncName, {}, issueId});
    }
    automationJobCv_.notify_one();
}

void AppController::ExecuteLuaGlobalAction(const std::string& name) {
    std::string callbackFuncName;
    const auto it =
        std::find_if(luaGlobalActions_.begin(), luaGlobalActions_.end(),
                     [&](const std::pair<std::string, std::string>& pair) { return pair.first == name; });
    if (it != luaGlobalActions_.end()) {
        callbackFuncName = it->second;
    }
    if (callbackFuncName.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(automationJobMutex_);
        automationJobs_.push_back({AutomationJob::Type::GlobalAction, callbackFuncName, {}, ""});
    }
    automationJobCv_.notify_one();
}

