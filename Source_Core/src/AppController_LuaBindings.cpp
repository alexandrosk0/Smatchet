#include "AppController.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "imgui.h"
#include "Logger.h"
#include "SmatchetFieldIconRender.h"
#include "StringUtil.h"
#include "TicketGridModel.h"
#include "TrackerFieldValueUtils.h"

namespace {

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
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

sol::environment CreateSandboxEnvironment(sol::state& lua) {
    sol::environment sandbox(lua, sol::create, lua.globals());
    sandbox["dofile"] = sol::lua_nil;
    sandbox["loadfile"] = sol::lua_nil;
    sandbox["load"] = sol::lua_nil;
    sandbox["loadstring"] = sol::lua_nil;
    sandbox["require"] = sol::lua_nil;
    sandbox["collectgarbage"] = sol::lua_nil;
    sandbox["os"] = sol::lua_nil;
    sandbox["io"] = sol::lua_nil;
    sandbox["package"] = sol::lua_nil;
    sandbox["debug"] = sol::lua_nil;
    return sandbox;
}

} // namespace

/** GCC + sol2: lambdas with the same signature can share one demangled metatable name → heap corruption in Lua.
 *  All InitLua callables here are plain functions with distinct symbols (plus scoped AppController* for Ticket glue). */
namespace smatchet_lua_init_detail {

/** Set in InitLua; cleared in ~AppController. Ticket userdata calls these without a per-call AppController*. */
AppController* gApp = nullptr;

std::tuple<bool, std::string> TicketSetFieldGlue(CachedTicket& t, const std::string& fieldId, const std::string& val) {
    if (gApp == nullptr) {
        return {false, "AppController not available for Ticket:set_field"};
    }
    const TrackerField* fieldMeta = gApp->FindFieldById(fieldId);
    if (!fieldMeta) {
        return {false, "Field not found in Jira catalog: " + fieldId};
    }
    std::string err;
    std::vector<std::string> vals;
    if (!val.empty()) {
        vals.push_back(val);
    }
    const bool ok = gApp->SubmitFieldEdit(t.id, *fieldMeta, vals, err);
    return {ok, err};
}

std::tuple<bool, std::string> TicketTransitionGlue(CachedTicket& t, const std::string& statusName) {
    if (gApp == nullptr) {
        return {false, "AppController not available for Ticket:transition"};
    }
    const TrackerField* statusField = gApp->FindFieldById("status");
    if (!statusField) {
        return {false, "Jira 'status' field meta not found"};
    }
    std::string err;
    const bool ok = gApp->SubmitFieldEdit(t.id, *statusField, {statusName}, err);
    return {ok, err};
}

void ImGuiSameLineGlue() {
    ImGui::SameLine();
}

void ImGuiSeparatorGlue() {
    ImGui::Separator();
}

void ImGuiProgressBarGlue(float fraction, float width, float height) {
    ImVec2 sz(width, height);
    if (width < 0.0f) {
        sz.x = ImGui::GetContentRegionAvail().x;
    }
    if (height <= 0.0f) {
        sz.y = ImGui::GetFrameHeight();
    }
    ImGui::ProgressBar(fraction, sz);
}

std::tuple<float, float> ImGuiGetContentRegionAvailGlue() {
    const ImVec2 v = ImGui::GetContentRegionAvail();
    return std::make_tuple(v.x, v.y);
}

bool ImGuiButtonGlue(const std::string& label) {
    return ImGui::Button(label.c_str());
}

void LuaLogInfoGlue(std::string msg) {
    gApp->LuaLogInfoBind(std::move(msg));
}

std::tuple<sol::object, std::string> LuaGetTicketGlue(const std::string& issueId) {
    return gApp->LuaGetTicketBind(issueId);
}

std::vector<CachedTicket> LuaGetActiveTicketsGlue() {
    return gApp->LuaGetActiveTicketsBind();
}

std::tuple<sol::object, std::string> LuaDecodeJsonGlue(const std::string& s) {
    return gApp->LuaDecodeJsonBind(s);
}

void LuaRegisterFieldDisplayGlue(const std::string& fieldId, sol::function fn) {
    gApp->LuaRegisterFieldDisplayBind(fieldId, std::move(fn));
}

void LuaUnregisterFieldDisplayGlue(const std::string& fieldId) {
    gApp->LuaUnregisterFieldDisplayBind(fieldId);
}

void LuaRegisterFieldDisplayByNameGlue(const std::string& displayName, sol::function fn) {
    gApp->LuaRegisterFieldDisplayByNameBind(displayName, std::move(fn));
}

void LuaUnregisterFieldDisplayByNameGlue(const std::string& displayName) {
    gApp->LuaUnregisterFieldDisplayByNameBind(displayName);
}

void LuaRegisterFieldIconMapGlue(const std::string& fieldKey, sol::table map, sol::optional<bool> byName) {
    gApp->LuaRegisterFieldIconMapBind(fieldKey, std::move(map), byName);
}

void LuaUnregisterFieldIconMapGlue(const std::string& fieldKey, sol::optional<bool> byName) {
    gApp->LuaUnregisterFieldIconMapBind(fieldKey, byName);
}

void LuaImGuiTextGlue(const std::string& s) {
    gApp->LuaImGuiTextBind(s);
}

void LuaImGuiTextUnformattedGlue(const std::string& s) {
    gApp->LuaImGuiTextUnformattedBind(s);
}

bool LuaImGuiImageGlue(const std::string& path, float w, float h) {
    return gApp->LuaImGuiImageBind(path, w, h);
}

void LuaUiRegisterWindowGlue(const std::string& name, sol::function drawFn) {
    gApp->LuaUiRegisterWindowBind(name, std::move(drawFn));
}

void LuaUiRegisterTicketActionGlue(const std::string& name, sol::function cb) {
    gApp->LuaUiRegisterTicketActionBind(name, std::move(cb));
}

void LuaUiRegisterGlobalActionGlue(const std::string& name, sol::function cb) {
    gApp->LuaUiRegisterGlobalActionBind(name, std::move(cb));
}

void LuaAiAddContextGlue(const std::string& text) {
    gApp->LuaAiAddContextBind(text);
}

void LuaAiPromptGlue(const std::string& message) {
    gApp->LuaAiPromptBind(message);
}

void LuaAiClearContextGlue() {
    gApp->ClearAiContext();
}

void LuaMcpRegisterToolGlue(sol::table toolDef, sol::function callback) {
    gApp->LuaMcpRegisterToolBind(std::move(toolDef), std::move(callback));
}

} // namespace smatchet_lua_init_detail

void AppController::InitLua() {
    lua.open_libraries(sol::lib::base);
    lua.open_libraries(sol::lib::string);
    lua.open_libraries(sol::lib::table);

    smatchet_lua_init_detail::gApp = this;

    lua.new_usertype<CachedTicket>("Ticket",
        "id", &CachedTicket::id,
        "get_field", &CachedTicket::GetFieldValue,
        "set_field", &smatchet_lua_init_detail::TicketSetFieldGlue,
        "transition", &smatchet_lua_init_detail::TicketTransitionGlue);

    sol::table smatchet = lua.create_table();
    smatchet.set_function("get_ticket", &smatchet_lua_init_detail::LuaGetTicketGlue);
    smatchet.set_function("get_active_tickets", &smatchet_lua_init_detail::LuaGetActiveTicketsGlue);
    lua["smatchet"] = smatchet;

    lua.set_function("log_info", &smatchet_lua_init_detail::LuaLogInfoGlue);

    lua.set_function("decode_json", &smatchet_lua_init_detail::LuaDecodeJsonGlue);

    lua.set_function("register_field_display", &smatchet_lua_init_detail::LuaRegisterFieldDisplayGlue);

    lua.set_function("unregister_field_display", &smatchet_lua_init_detail::LuaUnregisterFieldDisplayGlue);

    lua.set_function("register_field_display_by_name", &smatchet_lua_init_detail::LuaRegisterFieldDisplayByNameGlue);

    lua.set_function("unregister_field_display_by_name",
                     &smatchet_lua_init_detail::LuaUnregisterFieldDisplayByNameGlue);

    lua.set_function("register_field_icon_map", &smatchet_lua_init_detail::LuaRegisterFieldIconMapGlue);

    lua.set_function("unregister_field_icon_map", &smatchet_lua_init_detail::LuaUnregisterFieldIconMapGlue);

    sol::table imgui = lua.create_table();
    imgui.set_function("progress_bar", &smatchet_lua_init_detail::ImGuiProgressBarGlue);
    imgui.set_function("text", &smatchet_lua_init_detail::LuaImGuiTextGlue);
    imgui.set_function("text_unformatted", &smatchet_lua_init_detail::LuaImGuiTextUnformattedGlue);
    imgui.set_function("get_content_region_avail", &smatchet_lua_init_detail::ImGuiGetContentRegionAvailGlue);
    imgui.set_function("button", &smatchet_lua_init_detail::ImGuiButtonGlue);
    imgui.set_function("same_line", &smatchet_lua_init_detail::ImGuiSameLineGlue);
    imgui.set_function("separator", &smatchet_lua_init_detail::ImGuiSeparatorGlue);
    imgui.set_function("image", &smatchet_lua_init_detail::LuaImGuiImageGlue);
    lua["imgui"] = imgui;

    sol::table ui = lua.create_table();
    ui.set_function("register_window", &smatchet_lua_init_detail::LuaUiRegisterWindowGlue);
    ui.set_function("register_ticket_action", &smatchet_lua_init_detail::LuaUiRegisterTicketActionGlue);
    ui.set_function("register_global_action", &smatchet_lua_init_detail::LuaUiRegisterGlobalActionGlue);
    lua["ui"] = ui;

    sol::table ai = lua.create_table();
    ai.set_function("add_context", &smatchet_lua_init_detail::LuaAiAddContextGlue);
    ai.set_function("prompt", &smatchet_lua_init_detail::LuaAiPromptGlue);
    ai.set_function("clear_context", &smatchet_lua_init_detail::LuaAiClearContextGlue);
    lua["ai"] = ai;

    sol::table mcp = lua.create_table();
    mcp.set_function("register_tool", &smatchet_lua_init_detail::LuaMcpRegisterToolGlue);
    lua["mcp"] = mcp;
}

void AppController::LuaLogInfoBind(std::string msg) {
    const std::string clean = SanitizeLogText(msg);
    if (!AutomationLogSinks.empty()) {
        for (const auto& sink : AutomationLogSinks) {
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

void AppController::LuaRegisterFieldDisplayBind(const std::string& fieldId, sol::function fn) {
    if (fieldId.empty() || !fn.valid()) {
        return;
    }
    fieldDisplayHandlers_[fieldId] = sol::protected_function(std::move(fn));
}

void AppController::LuaUnregisterFieldDisplayBind(const std::string& fieldId) {
    fieldDisplayHandlers_.erase(fieldId);
}

void AppController::LuaRegisterFieldDisplayByNameBind(const std::string& displayName, sol::function fn) {
    if (displayName.empty() || !fn.valid()) {
        return;
    }
    fieldDisplayHandlersByDisplayName_[AsciiLowerCopy(displayName)] = sol::protected_function(std::move(fn));
}

void AppController::LuaUnregisterFieldDisplayByNameBind(const std::string& displayName) {
    fieldDisplayHandlersByDisplayName_.erase(AsciiLowerCopy(displayName));
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

void AppController::LuaUiRegisterWindowBind(const std::string& name, sol::function drawFn) {
    luaWindows_.erase(std::remove_if(luaWindows_.begin(), luaWindows_.end(),
                                     [&](const std::pair<std::string, sol::protected_function>& p) {
                                         return p.first == name;
                                     }),
                      luaWindows_.end());
    if (drawFn.valid()) {
        luaWindows_.push_back({name, sol::protected_function(std::move(drawFn))});
    }
}

void AppController::LuaUiRegisterTicketActionBind(const std::string& name, sol::function callback) {
    luaTicketActions_.erase(std::remove_if(luaTicketActions_.begin(), luaTicketActions_.end(),
                                         [&](const std::pair<std::string, sol::protected_function>& p) {
                                             return p.first == name;
                                         }),
                          luaTicketActions_.end());
    if (callback.valid()) {
        luaTicketActions_.push_back({name, sol::protected_function(std::move(callback))});
    }
}

void AppController::LuaUiRegisterGlobalActionBind(const std::string& name, sol::function callback) {
    luaGlobalActions_.erase(std::remove_if(luaGlobalActions_.begin(), luaGlobalActions_.end(),
                                         [&](const std::pair<std::string, sol::protected_function>& p) {
                                             return p.first == name;
                                         }),
                          luaGlobalActions_.end());
    if (callback.valid()) {
        luaGlobalActions_.push_back({name, sol::protected_function(std::move(callback))});
    }
}

void AppController::LuaAiAddContextBind(const std::string& text) {
    AddAiContext(text);
}

void AppController::LuaAiPromptBind(const std::string& message) {
    PromptAi(message);
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

    luaMcpTools_.erase(std::remove_if(luaMcpTools_.begin(), luaMcpTools_.end(),
                                      [&](const McpToolDefinition& d) { return d.name == def.name; }),
                      luaMcpTools_.end());

    luaMcpTools_.push_back(std::move(def));
}

void AppController::ClearLuaTicketContextGlue() {
    smatchet_lua_init_detail::gApp = nullptr;
}

void AppController::RunLuaSetupScript(const std::string& scriptPath) {
    auto logErr = [this](const char* prefix, const std::string& detail) {
        const std::string msg = std::string(prefix) + detail;
        if (!AutomationLogSinks.empty()) {
            for (const auto& sink : AutomationLogSinks) {
                sink(msg);
            }
        } else {
            std::printf("%s\n", msg.c_str());
        }
    };

    const std::string path = ResolveLuaScriptPath(scriptPath);
    if (path.empty()) {
        logErr("[LUA setup] ", "invalid script path");
        return;
    }
    
    sol::environment sandbox = CreateSandboxEnvironment(lua);
    sol::load_result script = lua.load_file(path);
    if (!script.valid()) {
        sol::error err = script;
        logErr("[LUA setup] ", err.what());
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
    }
}

bool AppController::TryLuaFieldDisplay(const std::string& fieldId, const CachedTicket& ticket,
                                       const std::string& rawValue, const float availWidth,
                                       const TrackerField* fieldMeta) {
    sol::protected_function* handler = nullptr;
    const auto itId = fieldDisplayHandlers_.find(fieldId);
    if (itId != fieldDisplayHandlers_.end() && itId->second.valid()) {
        handler = &itId->second;
    } else if (fieldMeta && !fieldMeta->Name.empty()) {
        const auto itName = fieldDisplayHandlersByDisplayName_.find(AsciiLowerCopy(fieldMeta->Name));
        if (itName != fieldDisplayHandlersByDisplayName_.end() && itName->second.valid()) {
            handler = &itName->second;
        }
    }
    if (handler == nullptr) {
        return false;
    }

    const bool catalogReadOnly = fieldMeta ? fieldMeta->ReadOnly : false;
    const bool editMetaReadOnly = !CanEditFieldForIssue(ticket.id, fieldId, fieldMeta);
    const bool isReadOnly = catalogReadOnly || editMetaReadOnly;
    sol::object fieldNameObj = fieldMeta ? sol::make_object(lua, fieldMeta->Name) : sol::make_object(lua, sol::nil);

    lua_sethook(lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) {
        luaL_error(L, "Script execution timeout exceeded.");
    }, LUA_MASKCOUNT, 10000);

    sol::protected_function_result pfr = (*handler)(ticket.id, fieldId, rawValue, availWidth, isReadOnly, fieldNameObj);

    lua_sethook(lua.lua_state(), nullptr, 0, 0);

    if (!pfr.valid()) {
        sol::error err = pfr;
        const std::string msg = std::string("[LUA field display] ") + err.what();
        if (!AutomationLogSinks.empty()) {
            for (const auto& sink : AutomationLogSinks) {
                sink(msg);
            }
        } else {
            std::printf("%s\n", msg.c_str());
        }
        return false;
    }

    if (pfr.return_count() < 1) {
        return false;
    }
    const sol::object ret = pfr.get<sol::object>(0);
    return LuaTruthy(ret);
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
            }
            if (!optId.empty()) {
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

void AppController::RunAutoScript(const std::string& scriptPath, const std::vector<std::string>& selectedIds) {
    const std::string path = ResolveLuaScriptPath(scriptPath);
    if (path.empty()) {
        LOG_WARN("RunAutoScript: invalid script path=%s", scriptPath.c_str());
        return;
    }

    sol::environment sandbox = CreateSandboxEnvironment(lua);
    sol::load_result script = lua.load_file(path);
    if (!script.valid()) {
        sol::error err = script;
        LOG_ERROR("RunAutoScript: lua error path=%s err=%s", path.c_str(), err.what());
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
        LOG_ERROR("RunAutoScript: init error path=%s err=%s", path.c_str(), err.what());
        return;
    }

    sol::protected_function process_func = sandbox["process_ticket"];
    if (!process_func.valid()) {
        LOG_WARN("RunAutoScript: script must define function process_ticket(ticket)");
        return;
    }

    lua_sethook(lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) {
        luaL_error(L, "Script execution timeout exceeded.");
    }, LUA_MASKCOUNT, 100000);

    const auto snap = GetActiveTicketsSnapshot();
    std::unordered_set<std::string> selectedSet(selectedIds.begin(), selectedIds.end());
    
    int processedCount = 0;
    for (auto& ticket : *snap) {
        if (!selectedSet.empty() && selectedSet.find(ticket.id) == selectedSet.end()) {
            continue; // Not in selection
        }
        if (selectedSet.empty()) {
            // If nothing is selected, we choose not to run it at all for safety,
            // or perhaps run on the actively focused ticket? 
            // The requirement says "applies on selected rows only". If none selected, do nothing.
            break;
        }

        sol::protected_function_result result = process_func(&ticket);
        if (!result.valid()) {
            sol::error e = result;
            LOG_ERROR("RunAutoScript: lua error ticket=%s path=%s err=%s", ticket.id.c_str(), path.c_str(), e.what());
        }
        processedCount++;
    }
    
    if (processedCount > 0) {
        LOG_INFO("RunAutoScript: processed %d tickets", processedCount);
    } else {
        LOG_WARN("RunAutoScript: no tickets were selected/processed");
    }
    
    lua_sethook(lua.lua_state(), nullptr, 0, 0);
}

std::vector<AppController::McpToolDefinition> AppController::GetLuaMcpTools() const {
    return luaMcpTools_;
}

std::string AppController::ExecuteLuaMcpTool(const std::string& name, const std::string& paramsJson, std::string& outError) {
    for (auto& tool : luaMcpTools_) {
        if (tool.name == name) {
            lua_sethook(lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) {
                luaL_error(L, "Script execution timeout exceeded.");
            }, LUA_MASKCOUNT, 100000);

            nlohmann::json jParams;
            try {
                jParams = nlohmann::json::parse(paramsJson);
            } catch (...) {
                jParams = nlohmann::json::object();
            }

            sol::protected_function_result result = tool.callback(JsonToLua(lua, jParams));
            
            lua_sethook(lua.lua_state(), nullptr, 0, 0);

            if (!result.valid()) {
                sol::error e = result;
                outError = e.what();
                return "";
            }
            if (result.return_count() > 0) {
                sol::object obj = result[0];
                if (obj.is<std::string>()) {
                    return obj.as<std::string>();
                } else {
                    return LuaToJson(obj).dump();
                }
            }
            return "{}";
        }
    }
    outError = "Tool not found";
    return "";
}

void AppController::DrawLuaWindows() {
    for (auto& pair : luaWindows_) {
        bool open = true;
        if (ImGui::Begin(pair.first.c_str(), &open)) {
            lua_sethook(lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) {
                luaL_error(L, "Script execution timeout exceeded.");
            }, LUA_MASKCOUNT, 100000);

            sol::protected_function_result res = pair.second();
            
            lua_sethook(lua.lua_state(), nullptr, 0, 0);

            if (!res.valid()) {
                sol::error e = res;
                ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Lua Error: %s", e.what());
            }
        }
        ImGui::End();
        if (!open) {
            // Unregister if user closes it
            pair.second = sol::lua_nil;
        }
    }
    
    // Clean up closed windows
    luaWindows_.erase(std::remove_if(luaWindows_.begin(), luaWindows_.end(),
        [](const std::pair<std::string, sol::protected_function>& p) { return !p.second.valid(); }), luaWindows_.end());
}

std::vector<std::string> AppController::GetLuaTicketActionNames() const {
    std::vector<std::string> names;
    for (const auto& pair : luaTicketActions_) {
        names.push_back(pair.first);
    }
    return names;
}

bool AppController::ExecuteLuaTicketAction(const std::string& name, const std::string& issueId, std::string& outError) {
    for (auto& pair : luaTicketActions_) {
        if (pair.first == name) {
            CachedTicket ticket;
            if (!Cache->TryGetTicket(issueId, ticket)) {
                outError = "Ticket not found in cache";
                return false;
            }
            lua_sethook(lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) {
                luaL_error(L, "Script execution timeout exceeded.");
            }, LUA_MASKCOUNT, 100000);

            sol::protected_function_result res = pair.second(sol::make_object(lua, ticket));
            
            lua_sethook(lua.lua_state(), nullptr, 0, 0);

            if (!res.valid()) {
                sol::error e = res;
                outError = e.what();
                return false;
            }
            return true;
        }
    }
    outError = "Action not found";
    return false;
}

void AppController::AddAiContext(const std::string& text) {
    aiContext_.push_back(text);
    if (aiContext_.size() > 100) {
        aiContext_.erase(aiContext_.begin());
    }
}

const std::vector<std::string>& AppController::GetAiContext() const {
    return aiContext_;
}

void AppController::ClearAiContext() {
    aiContext_.clear();
}

void AppController::PromptAi(const std::string& message) {
    if (aiPromptHandler_) {
        aiPromptHandler_(message);
    }
}

void AppController::SetAiPromptHandler(std::function<void(const std::string&)> handler) {
    aiPromptHandler_ = std::move(handler);
}

std::vector<std::string> AppController::GetLuaGlobalActionNames() const {
    std::vector<std::string> names;
    for (const auto& pair : luaGlobalActions_) {
        names.push_back(pair.first);
    }
    return names;
}

bool AppController::ExecuteLuaGlobalAction(const std::string& name, std::string& outError) {
    for (auto& pair : luaGlobalActions_) {
        if (pair.first == name) {
            lua_sethook(lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) {
                luaL_error(L, "Script execution timeout exceeded.");
            }, LUA_MASKCOUNT, 100000);

            sol::protected_function_result res = pair.second();

            lua_sethook(lua.lua_state(), nullptr, 0, 0);

            if (!res.valid()) {
                sol::error e = res;
                outError = e.what();
                return false;
            }
            return true;
        }
    }
    outError = "Action not found";
    return false;
}
