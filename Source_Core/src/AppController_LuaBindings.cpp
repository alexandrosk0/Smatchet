#include "AppController.h"

#include "ConfigManager.h"
#include "FieldEditAuditSource.h"
#include "IssueTableSerializer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <exception>
#include <future>
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

void LuaRegisterFieldDisplayGlue(sol::this_state L, const std::string& fieldId, sol::function fn) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaRegisterFieldDisplayBind(fieldId, std::move(fn));
}

void LuaUnregisterFieldDisplayGlue(sol::this_state L, const std::string& fieldId) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaUnregisterFieldDisplayBind(fieldId);
}

void LuaRegisterFieldDisplayByNameGlue(sol::this_state L, const std::string& displayName, sol::function fn) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaRegisterFieldDisplayByNameBind(displayName, std::move(fn));
}

void LuaUnregisterFieldDisplayByNameGlue(sol::this_state L, const std::string& displayName) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaUnregisterFieldDisplayByNameBind(displayName);
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
    AppController* app = ResolveApp(L);
    if (app) app->LuaImGuiTextBind(s);
}

void LuaImGuiTextUnformattedGlue(sol::this_state L, const std::string& s) {
    AppController* app = ResolveApp(L);
    if (app) app->LuaImGuiTextUnformattedBind(s);
}

bool LuaImGuiImageGlue(sol::this_state L, const std::string& path, float w, float h) {
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

    sol::table mcp = state.create_table();
    mcp.set_function("register_tool", &smatchet_lua_init_detail::LuaMcpRegisterToolGlue);
    state["mcp"] = mcp;
}

void AppController::InitLuaUi(sol::state& state) {
    state.set_function("register_field_display", &smatchet_lua_init_detail::LuaRegisterFieldDisplayGlue);
    state.set_function("unregister_field_display", &smatchet_lua_init_detail::LuaUnregisterFieldDisplayGlue);
    state.set_function("register_field_display_by_name", &smatchet_lua_init_detail::LuaRegisterFieldDisplayByNameGlue);
    state.set_function("unregister_field_display_by_name",
                     &smatchet_lua_init_detail::LuaUnregisterFieldDisplayByNameGlue);
    state.set_function("register_field_icon_map", &smatchet_lua_init_detail::LuaRegisterFieldIconMapGlue);
    state.set_function("unregister_field_icon_map", &smatchet_lua_init_detail::LuaUnregisterFieldIconMapGlue);

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
    ui.set_function("register_ticket_action", &smatchet_lua_init_detail::LuaUiRegisterTicketActionGlue);
    ui.set_function("register_global_action", &smatchet_lua_init_detail::LuaUiRegisterGlobalActionGlue);
    state["ui"] = ui;
}

void AppController::LuaLogInfoBind(const std::string& msg) {
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

            // Load setup scripts so global actions are defined
            for (const auto& path : setupScriptsSnapshot) {
                std::string resolved = ResolveLuaScriptPath(path);
                if (!resolved.empty()) {
                    auto script = bgState.load_file(resolved);
                    if (script.valid()) {
                        sol::protected_function func = script;
                        sandbox.set_on(func);
                        func();
                    }
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
        const std::string msg = std::string(prefix) + detail;
        LuaLogInfoBind(msg);
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

bool AppController::TryLuaFieldDisplay(const std::string& fieldId, const CachedTicket& ticket,
                                       const std::string& rawValue, const float availWidth,
                                       const TrackerField* fieldMeta) {
    sol::protected_function* handler = nullptr;
    const auto itId = fieldDisplayHandlers_.find(fieldId);
    if (itId != fieldDisplayHandlers_.end() && itId->second.valid()) {
        handler = &itId->second;
    } else if (fieldMeta && !fieldMeta->Name.empty() && !fieldDisplayHandlersByDisplayName_.empty()) {
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
    for (auto& pair : luaWindows_) {
        bool open = true;
        if (ImGui::Begin(pair.first.c_str(), &open)) {
            FieldEditAuditSource::ScopedOverride luaSource(FieldEditAuditSource::kLua);
            lua_sethook(lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) {
                luaL_error(L, "Script execution timeout exceeded.");
            }, LUA_MASKCOUNT, 100000);

            sol::protected_function_result res = pair.second();
            
            lua_sethook(lua.lua_state(), nullptr, 0, 0);

            if (!res.valid()) {
                sol::error e = res;
                LOG_TRACE("DrawLuaWindows: error window=%s %s", pair.first.c_str(), e.what());
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

