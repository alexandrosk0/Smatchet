#include "AppController.h"

#include <cctype>
#include <cstdio>
#include <exception>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "imgui.h"
#include "Logger.h"

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

sol::object JsonToLua(sol::state_view luaView, const nlohmann::json& j) { return JsonToLuaImpl(luaView, j, 0); }

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

} // namespace

void AppController::InitLua() {
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table);
    lua["dofile"] = sol::lua_nil;
    lua["loadfile"] = sol::lua_nil;
    lua["load"] = sol::lua_nil;
    lua["loadstring"] = sol::lua_nil;
    lua["require"] = sol::lua_nil;
    lua["collectgarbage"] = sol::lua_nil;
    lua["os"] = sol::lua_nil;
    lua["io"] = sol::lua_nil;
    lua["package"] = sol::lua_nil;
    lua["debug"] = sol::lua_nil;

    lua.new_usertype<CachedTicket>("Ticket", "id", &CachedTicket::id, "get_field", &CachedTicket::GetFieldValue);

    lua.set_function("log_info", [this](std::string msg) {
        const std::string clean = SanitizeLogText(msg);
        if (!AutomationLogSinks.empty()) {
            for (const auto& sink : AutomationLogSinks) {
                sink(clean);
            }
        } else {
            std::printf("[LUA] %s\n", clean.c_str());
        }
    });

    lua.set_function("decode_json", [this](const std::string& s) -> std::tuple<sol::object, std::string> {
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
    });

    lua.set_function("register_field_display", [this](const std::string& fieldId, sol::function fn) {
        if (fieldId.empty() || !fn.valid()) {
            return;
        }
        fieldDisplayHandlers_[fieldId] = sol::protected_function(std::move(fn));
    });

    lua.set_function("unregister_field_display",
                     [this](const std::string& fieldId) { fieldDisplayHandlers_.erase(fieldId); });

    lua.set_function("register_field_display_by_name", [this](const std::string& displayName, sol::function fn) {
        if (displayName.empty() || !fn.valid()) {
            return;
        }
        fieldDisplayHandlersByDisplayName_[AsciiLowerCopy(displayName)] = sol::protected_function(std::move(fn));
    });

    lua.set_function("unregister_field_display_by_name", [this](const std::string& displayName) {
        fieldDisplayHandlersByDisplayName_.erase(AsciiLowerCopy(displayName));
    });

    sol::table imgui = lua.create_table();
    imgui.set_function("progress_bar", [](float fraction, float width, float height) {
        ImVec2 sz(width, height);
        if (width < 0.0f) {
            sz.x = ImGui::GetContentRegionAvail().x;
        }
        if (height <= 0.0f) {
            sz.y = ImGui::GetFrameHeight();
        }
        ImGui::ProgressBar(fraction, sz);
    });
    imgui.set_function("text", [](const std::string& s) { ImGui::TextUnformatted(s.c_str()); });
    imgui.set_function("text_unformatted", [](const std::string& s) { ImGui::TextUnformatted(s.c_str()); });
    imgui.set_function("get_content_region_avail", []() {
        const ImVec2 v = ImGui::GetContentRegionAvail();
        return std::make_tuple(v.x, v.y);
    });
    lua["imgui"] = imgui;
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
    try {
        lua.script_file(path);
    } catch (const sol::error& e) {
        logErr("[LUA setup] ", e.what());
    } catch (const std::exception& e) {
        logErr("[LUA setup] ", e.what());
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

    sol::table ctx = lua.create_table();
    ctx["issue_id"] = ticket.id;
    ctx["field_id"] = fieldId;
    ctx["raw"] = rawValue;
    ctx["avail_width"] = availWidth;
    const bool catalogReadOnly = fieldMeta ? fieldMeta->ReadOnly : false;
    const bool editMetaReadOnly = !CanEditFieldForIssue(ticket.id, fieldId, fieldMeta);
    ctx["read_only"] = catalogReadOnly || editMetaReadOnly;
    if (fieldMeta) {
        ctx["field_name"] = fieldMeta->Name;
    } else {
        ctx["field_name"] = sol::lua_nil;
    }

    sol::protected_function_result pfr = (*handler)(ctx);
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

void AppController::RunAutoScript(const std::string& scriptPath) {
    const std::string path = ResolveLuaScriptPath(scriptPath);
    if (path.empty()) {
        LOG_WARN("RunAutoScript: invalid script path=%s", scriptPath.c_str());
        return;
    }
    const auto snap = GetActiveTicketsSnapshot();
    std::vector<CachedTicket> tickets(snap->begin(), snap->end());
    for (auto& ticket : tickets) {
        lua["ticket"] = &ticket;
        try {
            lua.script_file(path);
        } catch (const sol::error& e) {
            LOG_ERROR("RunAutoScript: lua error ticket=%s path=%s err=%s", ticket.id.c_str(), path.c_str(), e.what());
        } catch (const std::exception& e) {
            LOG_ERROR("RunAutoScript: exception ticket=%s path=%s err=%s", ticket.id.c_str(), path.c_str(), e.what());
        }
    }
}
